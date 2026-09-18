#pragma once

// xphone-os M2 — companion BLE peripheral, ported from x4-os
// src/companion/CompanionBleService.h (copied, not symlinked; x4-os is
// read-only reference). Differences from the x4-os original:
//   * <Logging.h> replaced by the local BleShim.h Serial-printf macros.
//   * Camera image transfer (CompanionCameraImageState + buffer fields,
//     sendCameraPreview/sendCameraRequest) stripped — camera.image.* card
//     writes are dropped with a status message, everything else in the
//     card/command JSON protocol and all UUIDs are unchanged so the existing
//     iOS companion app connects as before.
//   * Card payload parsing moved OFF the BLE host task: handleCardWrite()
//     (NimBLE host callback) only copies the raw bytes into a small FIFO;
//     processPending() — called from the Arduino main loop — runs the JSON
//     parse (x4-os learning: a Block card parsed on the nimble_host stack
//     caused a Stack protection fault; docs/x4-core-learnings.md "BLE
//     callback stability learning").
//   * Peer address capture (getPeerAddress) for the About scene, and
//     connect/disconnect/auth-complete forwarding into CompanionAncsClient.

#include <cstddef>
#include <cstdint>
#include <string>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "CompanionProtocol.h"

class BLEAdvertising;
class BLECharacteristic;
class BLEService;
class BLEDescriptor;
class BLECharacteristicCallbacks;
class BLEServer;
class BLEServerCallbacks;
struct ble_gap_conn_desc;

class CompanionBleService final {
 public:
  void begin();
  void startAdvertising();
  /** After a firmware update that changed the GATT table, tell bonded phones once (Service Changed). */
  void announceGattTableIfChanged(bool force = false);
  // The Bluetooth slow lane asks the phone for a fast link while a book is
  // in flight (15-30 ms, no latency) and hands the low-duty set back after.
  // Android sets its own priority; iOS honours only a request from us, and
  // on the ANCS low-duty link (180 ms, latency 4) a book crawled at 0.7 KB/s
  // (X3 + iPhone, 2026-09-05 16:45). Main loop only (ble_gap_update_params).
  void requestLaneLink(bool fast);
  void stopAdvertising();
  // R2 File Transfer — stop the NimBLE stack for the Wi-Fi session. The
  // X3 idles at ~39 KB free heap with BLE+ANCS up and Wi-Fi bring-up needs
  // ~50 KB, so they cannot coexist (measured: WiFi.mode aborted the
  // firmware). Since 2026-09-04 the stack stops WITHOUT releasing the
  // controller region (release=0, the reader's path), so the session can
  // end with resumeAfterTransfer() instead of a reboot. Measured cost of
  // keeping the region: ~1.5 KB in transfer mode (transfer scope doc).
  void shutdownForTransfer();
  // No-restart transfer exit: bring BLE back after Wi-Fi is fully off, and
  // queue one transfer.status for the phone's first write after it
  // reconnects (state may be null: nothing queued, e.g. when an NVS failure
  // breadcrumb already waits for that slot).
  void resumeAfterTransfer(const char* pendingState, const char* pendingDetail);
  // A transfer status to send on the phone's first write after this boot:
  // the post-sync restart (2026-09-07) cannot send "stopped" itself.
  void queueTransferStatus(const char* state, const char* detail);

  // Reader <-> radio time-sharing (CrossPoint's model). The X3 has no PSRAM;
  // with BLE+ANCS resident the reader can't get its 32 KB inflate window +
  // parser buffers, so first-time chapter indexing fails. The Reader scene
  // suspends BLE on entry (deinit keeps memory so no reboot is needed) and
  // resumes it on exit.
  void suspendForReader();
  void resumeAfterReader();
  // Free every heap block this service still owns from the connected era
  // (card JSON mirrors, pending inbound slices, status string). Runs as the
  // tail of suspendForReader(): tiny survivors otherwise sit mid-heap and
  // split the contiguous 32 KB the reader needs (measured on X3). All of it
  // is recoverable — parsed stores are fixed buffers, NVS keeps the last
  // real card, the phone re-pushes on reconnect.
  // radioUp=true: the FBP reading path, where the link stays connected. Frees
  // the same connected-era heap (the parse slot, raw card JSON, pending
  // slices) but keeps the status text; the phone re-pushes cards on the next
  // sync. Without this the FBP path never freed them (efficiency audit
  // 2026-09-02: the pinned survivors that split the reading heap).
  void releaseReaderTransients(bool radioUp = false);

  bool isStarted() const { return started; }
  bool isConnected() const;
  // True while the radio is advertising (fast or slow window). Used by the
  // power-bench trace flags.
  bool isAdvertising() const;
  // Bench: terminate every open link (the phone sees a dropout and runs its
  // normal reconnect path). Used by the devcon 'bledrop' lever.
  void dropLinks();
  // Radio knob: keep advertising for a second phone while one is connected
  // (default on = today's behavior). Off stops advertising on connect.
  void setAdvertiseWhileConnected(bool on) { advWhileConnected = on; }
  bool advertiseWhileConnected() const { return advWhileConnected; }
  uint32_t getRevision() const;
  std::string getStatusMessage() const;
  // NOTE deliberately no getCard()/getBlockCard(): scenes render from the
  // bounded fixed stores (BLOCK_STATUS/TODAY_STORE/...), never by copying the
  // 3.6 KB parse slot. `card` below is the parse/assembly scratch only.

  // Last Today / Priorities card JSON (raw payload), stashed so Sleep can persist
  // them to NVS and re-seed the stores on wake — skipping the blank "Syncing"
  // screen. Main-loop only (set in applyCardPayload). "" until one arrives.
  // (Priorities persists from PRIORITIES_STORE instead — a multi-part
  // snapshot's last raw payload is only the tail slice.)
  const std::string& getLastTodayCard() const { return lastTodayCardJson; }
  const std::string& getLastWorkoutCard() const { return lastWorkoutCardJson; }
  // Re-apply a persisted card JSON at boot to seed a store before BLE is up.
  void seedPersistedCard(const std::string& json) {
    if (!json.empty()) applyCardPayload(json);
  }
  // Copies the last connected peer's OTA address ("aa:bb:cc:dd:ee:ff") into
  // buf; returns false (buf = "") when no peer has connected since boot.
  bool getPeerAddress(char* buf, std::size_t bufSize) const;

  // M2.1b (power lever 1) — advertising duty policy. Fast (30-60 ms, the
  // NimBLE connectable default band) for kAdvFastWindowMs after boot or a
  // disconnect for quick-discovery UX, then slow (400-500 ms) indefinitely.
  enum class AdvMode : uint8_t { Fast, Slow };
  AdvMode getAdvMode() const;
  // Main-loop tick: demotes fast -> slow advertising once the window expires
  // (restart of advertising must not run on the NimBLE host task).
  void tickAdvPolicy();

  // Advertising watchdog (0.7 item 7, 2026-09-06). The bench X3 advertised
  // for two hours (11:52-13:50) that no phone could see, after a disconnect;
  // a chip reset healed it. Two things were wrong before this: the restart
  // after a disconnect and the 60 s demotion both called start() through a
  // path that swallowed a refusal, so a silent radio left no log line and
  // nothing ever retried. Main-loop tick. While advertising is wanted and no
  // phone is connected ("unseen"):
  //   every 5 s  the stack says it is not advertising -> start it again;
  //   after P    stop/start advertising, back in the fast window (stage 1);
  //   after 2P   cycle the Bluetooth controller through the reader's own
  //              suspend/resume path, once per stretch, bonded only (stage 2);
  //   after 3P   ask main.cpp for a reboot from an idle screen, once per
  //              phone absence (NVS mark, cleared on connect), bonded only
  //              (stage 3); after that only the free stage-1 restart, every P.
  // P is 30 min. Bench: devcon 'advwd <min>' sets P, 'advwd now' skips the
  // wait for the next stage, 'advwd' prints the state.
  void tickAdvWatchdog();
  void setAdvWatchdogPeriodMin(uint32_t minutes);
  void forceAdvWatchdogStage() { advWdForce = true; }
  bool advWatchdogWantsReboot() const { return advWdRebootWanted; }
  // Writes the once-per-absence mark to NVS and restarts the chip.
  void advWatchdogRebootNow();
  void advWatchdogStatus(char* buf, std::size_t n) const;

  // M2.1b (power lever 2 readout) — live parameters of the current
  // connection from the NimBLE descriptor (ble_gap_conn_find). Units are raw
  // BLE: itvl125 x 1.25 ms, timeout10ms x 10 ms. False when not connected.
  bool getConnParams(uint16_t& itvl125, uint16_t& latency, uint16_t& timeout10ms) const;

  void updateStatus(const std::string& message);
  void publishCard(const CompanionCardState& next, const std::string& message);
  bool sendBlockStart(uint16_t minutes = 0, const char* presetId = nullptr);
  bool sendBlockBreak(uint16_t minutes = 5);
  bool sendBlockStop();
  bool sendBlockStatus();
  // M3 Priorities — same command JSON the x4-os service sends (x4-os
  // CompanionBleService.cpp:915-918/984-1014); iOS answers both with a fresh
  // "priorities.snapshot" card (PrioritiesManager.swift handleActionPayload).
  bool sendPrioritiesSyncRequest();
  bool sendPriorityToggle(const char* itemId, bool done);
  bool sendWorkoutSyncRequest();
  // Absolute completed-set count for one exercise (idempotent on the phone).
  bool sendWorkoutSet(const char* itemId, int done);
  // M3 Today — "today.sync.request" command, same generic command JSON shape
  // as the priorities sync request. NOTE: x4-os TodayActivity never sends a
  // sync (its Sync button is a placeholder no-op) and the current iOS app has
  // no handler for this type — unknown types fall through its dispatch
  // harmlessly (BluetoothManager.swift didUpdateValueFor), so this is the
  // forward-compatible hook for when the iOS producer lands.
  bool sendTodaySyncRequest();
  bool sendAction(std::size_t actionIndex);
  void markConnected(bool value);
  void setPeerAddress(const uint8_t* addrLe);  // 6 bytes, little-endian (NimBLE order)

  // Pairing/encryption pump. iOS shows its pairing popup only once the
  // peripheral initiates security — the framework's automatic
  // startSecurity-on-connect path does not reliably do so on hardware, so
  // the connect callback arms this and processPending() (main loop) calls
  // ble_gap_security_initiate with logging and one retry.
  void armSecurity(uint16_t connHandle);  // NimBLE host task: state flips only
  void disarmSecurity();                  // NimBLE host task: state flips only
  // Called from the security callback's onAuthenticationComplete (host task)
  // before ANCS sees the event; state flips + status only, no BLE calls.
  void handleEncryptionChange(ble_gap_conn_desc* desc);
  bool isEncrypted() const;

  // BLE host-task side: stash the raw write, never parse here.
  void handleCardWrite(BLECharacteristic* characteristic);
  void handleFileWrite(BLECharacteristic* characteristic);  // slow lane frames (host task)
  // Main-loop side: parse at most one stashed payload per call.
  void processPending();

  // R2 Read — phone-initiated requests, latched by applyCardPayload (main
  // loop) and consumed by main.cpp's pump. No mutex needed: both sides run
  // on the main loop.
  enum class TransferRequest : uint8_t { None, Start, StartDirect, Stop };
  // The route the phone asked for with transfer.start (2026-09-04, "only
  // the network the phone is on"): the phone's current SSID, and for the
  // phone-hotspot rung a session-only password. Empty ssid = the old walk
  // over every saved network (old apps, the bench `sta`). Copied out by
  // the scene with takeTransferTarget().
  // `hotspotFallback` comes back true when the phone said it could not name
  // its network; read and cleared with the target so the two cannot drift.
  bool takeTransferTarget(char* ssid, size_t ssidSize, char* pass, size_t passSize,
                          bool* hotspotFallback = nullptr);
  // Bench: devcon 'sta <ssid>' plants a target as if the phone had named it.
  void setTransferTarget(const char* ssid, const char* pass);
  // Bench: devcon 'stafb' plants the "I could not name my network" flag with
  // no target, the shape a phone sends when its OS withholds the name.
  void setTransferHotspotFallback(bool on);
  bool consumeShelfRequest();
  // Same latch, for reading progress + stats.
  bool consumeProgressRequest();
  bool consumeWifiKnownRequest();  // W1: app asked for the network report
  TransferRequest consumeTransferRequest();

  // Item 6: a reading place pushed from a phone over the (encrypted) link.
  // Latched here by applyCardPayload; the main loop writes the matching
  // book's .pos so the next open resumes there. The reader suspends BLE
  // while open, so a push only ever arrives off the reading screen — no
  // live-jump into an open book is needed.
  struct PlacePush {
    char key[64];       // canonical book key
    uint32_t page;
    uint32_t pageCount;
    uint64_t seq;
    uint64_t atMillis;
  };
  bool consumePlacePush(PlacePush& out);
  // X1 reader.goto: {canonical key, paragraph content id}.
  struct GotoPush {
    char key[64];
    uint32_t cid;
  };
  bool consumeGotoPush(GotoPush& out);
  void benchInjectGoto(const char* key, uint32_t cid);
  // Bench: inject a place push through the same latch the card handler
  // uses, so the write+resume path is provable without the app's sender.
  void benchInjectPlace(const char* key, uint32_t page, uint32_t pageCount);

  // Item 6 step 3, OUTBOUND. The reader suspends BLE while open, so the
  // moment the phone can hear where the device got to is book-close, when
  // the radio comes back. ReaderScene::onExit queues the last-read place
  // here; the pump sends it once the link is up and encrypted.
  void queueReaderPlace(const char* key, uint32_t page, uint32_t pageCount);
  // Main loop: if a place is queued and the link is encrypted, notify it.
  void pumpReaderPlace();

  // F1: the LIVE position stream — the deliberate opposite of the place
  // record above. One slot, last turn wins, at most one notify per second,
  // plus a 5-minute heartbeat while the book stays open (so a slow reader
  // keeps their phone-side session alive). Lossy by contract: the phone
  // must never feed reader.pos into the saved-place resolver.
  void notifyReaderHl(const char* key, uint32_t cid, uint32_t day, bool removed);
  void queueReaderPos(const char* key, uint16_t page, uint16_t count, uint32_t cid, uint16_t min);
  void clearReaderPos();  // book closed: stream and heartbeat stop
  void pumpReaderPos();   // main loop
  // Set when the phone writes (proving it is subscribed); cleared on
  // disconnect. Gates the outbound place past the re-subscription race.
  void notePhoneGone();
  // GAP SUBSCRIBE events, routed from the gap hook: the truth about whether
  // the action channel has a live listener this connection.
  void noteActionSubscribe(uint16_t connHandle, uint16_t attrHandle, bool curNotify);
  void noteConnHandle(uint16_t connHandle);

  // Scan /books on the SD card and notify it as chunked "reader.shelf"
  // JSON messages sized to the live ATT MTU (a notify larger than MTU-3 is
  // silently truncated by NimBLE, which would corrupt the JSON). Main loop
  // only (SD access + notifies).
  void sendReaderShelf();
  // W1: saved network names + last-scan sightings + any pending join
  // failure (read-and-clear), as one chunked notify.
  void sendWifiKnown();
  // Reading progress + lifetime stats, chunked like the shelf. This is what
  // makes the app correct whenever you open it: without it, progress only
  // reaches the phone during a Wi-Fi transfer session started by hand.
  void sendReaderProgress();
  // Notify the phone with the ANCS app-name cache as chunked "notif.apps"
  // messages. Main-loop-only for the same reason as the cache enumerator.
  void sendNotifApps();
  // Phase 3: installed-app inventory as chunked "device.apps" notifies.
  void sendAppsInventory();
  // Phase 4: firmware identity for the phone's update check —
  // {version, gitRev, device, slot, otaPending, battery}. One notify.
  void sendDeviceInfo();
  // {"type":"transfer.status","state":...,"ip":...,"detail":...} notify —
  // how the File Transfer scene reports Wi-Fi progress back to the phone.
  void sendTransferStatus(const char* state, const char* ip = nullptr, const char* detail = nullptr);
  // Device Wi-Fi screen (2026-09-04): "wifi.request" = the user pressed
  // "Add a network from the app" (the app opens its Wi-Fi page);
  // "wifi.forgot" = the user forgot a network on the device (the app drops
  // it from its vault too, or the next reconcile would push it back).
  void sendWifiRequest();
  void sendWifiForgot(const char* ssid);
  // "wifi.test": the device is about to join <ssid> as a test; the app
  // probes for it for ~30 s (no upload) so the reach test can succeed.
  void sendWifiTest(const char* ssid);

 private:
  void shutdownRadio(bool releaseMemory, const char* reason);

  bool started = false;
  bool advertisingWanted = false;
  bool connected = false;
  uint32_t revision = 0;
  uint32_t actionSequence = 0;
  // Parse/assembly scratch: incoming cards land here (multi-part snapshots
  // accumulate across parts), the fixed stores capture what scenes render,
  // and releaseReaderTransients() frees its string heap at reader suspend.
  CompanionCardState card;
  std::string lastTodayCardJson;       // raw JSON of the last today.snapshot card
  std::string lastWorkoutCardJson;     // raw JSON of the last workout.snapshot card
  std::string statusMessage = "Not advertising";
  char peerAddress[18] = {0};
  // Queued by resumeAfterTransfer(), delivered at the next time.sync (the
  // phone's first write after it reconnects) — a notify sent any earlier
  // raced the re-subscription (bench, 2026-08-22). Guarded by stateMutex.
  char pendingTransferState[16] = {0};
  char pendingTransferDetail[40] = {0};
  char transferTargetSsid[64] = {0};
  char transferTargetPass[64] = {0};
  // "fallback": the phone is on Wi-Fi but its OS would not name the network,
  // so it cannot give us a target. Walk the saved list as before, but treat
  // an unreachable session the way a named target does and raise the hotspot
  // rather than fail. Without this the hotspot is unreachable on BOTH sides
  // at once whenever the name is withheld. (2026-09-10)
  bool transferHotspotFallback = false;

  // Raw card JSON queued by handleCardWrite(). A small FIFO, not a single
  // slot: multi-part priorities snapshots arrive one GATT write per
  // connection interval, and a single slot dropped earlier parts whenever
  // the main loop was busy composing a frame between drains.
  static constexpr std::size_t PENDING_CAPACITY = 4;
  std::string pendingPayloads[PENDING_CAPACITY];
  std::size_t pendingHead = 0;   // next slot to drain
  std::size_t pendingCount = 0;  // filled slots

  // R2 Read — main-loop-only latches (see consumeShelfRequest above).
  bool shelfRequested = false;
  bool progressRequested = false;
  // Non-zero while a chunked send waits for the MTU exchange (see
  // sendReaderProgress / sendWifiKnown): consumeProgressRequest holds off.
  uint32_t progressRetryAtMs = 0;
  bool wifiKnownRequested = false;
  TransferRequest transferRequest = TransferRequest::None;
  bool placePushPending = false;
  PlacePush pendingPlace{};
  GotoPush pendingGoto{};
  bool gotoPushPending = false;
  bool outPlacePending = false;
  PlacePush pendingOut{};
  // F1 stream slot (main-loop only, like pendingOut).
  struct PosStream {
    char key[64];
    uint16_t page;
    uint16_t count;
    uint32_t cid;
    uint16_t min;
  };
  PosStream posOut{};
  bool posValid = false;      // a book is open and has reported a position
  bool posDirty = false;      // an unsent change
  uint32_t posLastSentMs = 0;
  uint32_t placeSeq = 0;  // monotonic within a boot; orders our own sends
  bool phoneReadyForNotify = false;
  // iOS NEVER re-writes the CCCD for a bonded peer — the spec says the
  // device must remember it, and ours forgets on every reboot. When no
  // subscribe arrived but the link is encrypted, notifyAction() sends
  // straight through the stack so iOS's assumption is true. (Found
  // 2026-08-23: three healthy connections, zero notifications received.)
  bool actionSubscribed = false;
  uint16_t encConnHandle = 0xFFFF;
  bool advWhileConnected = false;  // Andrew 2026-09-03: no second-phone advertising while connected (each beacon wakes the radio)
  void notifyAction();

  // Security pump state — written from the NimBLE host task (arm/disarm/
  // encryption-change) and the main loop (processPending), so every touch
  // holds stateMutex.
  uint16_t secConnHandle = 0xffff;
  // The handle encryption actually completed on, so armSecurity() can tell a
  // fresh link from one the stack already secured. A bonded iOS reconnect
  // encrypts BEFORE the connect callback runs, and armSecurity used to reset
  // `encrypted` to false regardless — see the comment there.
  uint16_t securedConnHandle = 0xffff;
  bool encrypted = false;
  bool securityPending = false;
  uint8_t securityAttempts = 0;
  uint32_t securityDueAtMs = 0;

  // M2.1b adv policy state — advMode/advFastUntilMs are written from the
  // main loop (begin/tickAdvPolicy) and the host task (markConnected on
  // disconnect), so every touch holds stateMutex.
  AdvMode advMode = AdvMode::Fast;
  uint32_t advFastUntilMs = 0;

  // Advertising watchdog state — main loop only (tickAdvWatchdog, devcon).
  uint32_t advWdPeriodMs = 30UL * 60UL * 1000UL;
  uint32_t advWdStretchStartMs = 0;  // 0 = no unseen stretch running
  uint32_t advWdStageAtMs = 0;       // when the current stage began
  uint32_t advWdLastProbeMs = 0;     // last 5 s "is the stack advertising" probe
  uint32_t advWdLastKickMs = 0;      // last probe-driven start()
  uint16_t advWdKicks = 0;           // probe-driven starts this boot
  uint8_t advWdStage = 0;            // 0 waiting, 1 adv restarted, 2 controller cycled, 3 reboot asked
  bool advWdForce = false;
  bool advWdRebootWanted = false;
  bool advWdMarkClearedThisLink = false;

  mutable SemaphoreHandle_t stateMutex = nullptr;

  BLEServer* server = nullptr;
  // Kept ONLY so shutdownRadio can free them after BLEDevice::deinit:
  // upstream BLEServer has no destructor, so deinit deletes the server but
  // orphans the service/characteristic objects each begin() creates —
  // measured 1.76 KB leaked per reader BLE cycle (X4 bench, 2026-08-11).
  BLEService* gattService = nullptr;
  BLECharacteristic* cardCharacteristic = nullptr;
  BLEAdvertising* advertising = nullptr;
  BLECharacteristic* actionCharacteristic = nullptr;
  BLECharacteristic* fileCharacteristic = nullptr;
  BLEDescriptor* actionCccd = nullptr;  // the 0x2902 on the action characteristic; freed with it

  void ensureMutex() const;
  void applyAdvIntervals(AdvMode mode);  // writes m_advParams only; applied at next start()
  void setStatus(const std::string& message);
  bool sendBlockCommand(const char* type, uint16_t minutes, const char* presetId = nullptr);
  bool sendCommand(const char* type);
  bool applyCardPayload(const std::string& payload);
};

extern CompanionBleService COMPANION_BLE;
