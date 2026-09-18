#include "../TransferMemoryProbe.h"
#include "../BenchFramebufferLoan.h"
#include "../TransferSync.h"
#include <Preferences.h>
#include "FileTransferScene.h"
#include "../StackProbe.h"
#include <esp_heap_caps.h>

#include <ESPmDNS.h>
#include <InflateReader.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <lwip/tcpip.h>
#include <lwip/tcp.h>
#include <lwip/priv/tcp_priv.h>
#include <esp_system.h>


#include <cstdio>
#include <cstring>

#include <qrcodegen.h>

#include "../Fonts.h"
#include "../ble/CompanionBleService.h"
#include "../net/WifiCreds.h"
#include "AppScenes.h"
#include "WifiTest.h"
extern int8_t gWifiTxPowerQuarterDb;  // devcon txpwr (main.cpp); 0 = driver default

bool gWifiTestMode = false;
char gWifiTestResult[96] = {0};
uint32_t gWifiTestAtMs = 0;
char gWifiTestSsid[64] = {0};

// net/FileTransferServer.cpp — the W3 session token, RAM only.
void transferSetSessionToken(const char* token, bool ownedReadiness);
// main.cpp devcon 'wifibad': the next join uses a wrong password (bench).
bool gTransferBadPassword = false;

namespace {
constexpr int kMarginX = 20;  // matches SettingsScene / CrossPoint contentSidePadding
constexpr int kHeaderH = 46;

constexpr uint32_t kStaTimeoutMs = 20000;
// How long a radio-down failure lingers on glass before the session ends
// by itself and BLE returns (long enough to read; short enough that the phone's
// wait feels like a hiccup, not a hang).
constexpr uint32_t kFailedLingerMs = 6000;
// CrossPoint pumps up to 500 handleClient calls per activity loop; our main
// loop already ticks every 10 ms and one call drains one full request
// synchronously, so a small burst per tick is enough.
constexpr int kPumpPerTick = 8;
// W2: how long the hotspot waits for a phone before giving the radio back.
// The phone's join dialog is a human step; two minutes is generous for it
// and short enough that a dismissed dialog cannot flatten the battery.
constexpr uint32_t kApNoClientTimeoutMs = 120000;
static bool sSilentNow = false;  // the progress hook must not repaint behind the pill
// Same patience for a LAN session that goes quiet. A phone that dies
// mid-upload used to strand the device in Wi-Fi mode for ever, holding BLE
// down. Generous, because a big book can pause between HTTP requests while
// the phone reads it off storage. (2026-08-19)
constexpr uint32_t kStaIdleTimeoutMs = 180000;
// Parked = on this scene with NO session running (Idle, or the needs-wifi
// RETRY screen). BLE stays up in these states, so nothing is lost by
// leaving; staying forever is what ate start requests on an unattended
// reader.
constexpr uint32_t kParkedTimeoutMs = 120000;
// Device-side reach test (2026-09-04): joined a shared network, server up,
// and no HTTP request in this long -> the phone cannot reach us here (a
// guest network with client isolation, or a 5 GHz-only phone). Become the
// hotspot instead. The phone gives the shared path about the same budget.
constexpr uint32_t kKnockTimeoutMs = 20000;
// On the PHONE's hotspot (a session-only network, 2026-09-04) the phone is
// the gateway and has no other way to learn our address: mDNS may not
// cross its hotspot interface. Say where we are, to the gateway, on
// CrossPoint's discovery port, every 2 s until the first request.
constexpr uint16_t kAnnouncePort = 8134;
constexpr uint32_t kAnnounceEveryMs = 2000;

constexpr const char* kHostname = "xphone";

// Last 802.11 disconnect reason the STA event hook saw (2=AUTH_EXPIRE,
// 15/204=4-way handshake i.e. wrong password, 201=NO_AP_FOUND). Written by
// the WiFi event task, read by the scene tick — volatile is enough for a
// single int flag.
volatile int gLastStaDisconnectReason = 0;
}  // namespace


namespace {
bool sSyncDisplay = false;
bool sHandlingHttp = false;
bool sCancelSync = false;
}
namespace transfer_sync {
bool active() { return sSyncDisplay; }
bool memoryReleased() { return sSyncDisplay && G_GFX && !G_GFX->display().getFrameBuffer(); }
bool handlingHttp() { return sHandlingHttp; }
void setHandlingHttp(bool active) { sHandlingHttp = active; }
void requestCancel() { sCancelSync = true; }
bool cancelRequested() { return sCancelSync; }
}

bool FileTransferScene::activateSyncMemory(bool associatedHotspot) {
  if (_state != State::Running || !G_GFX) return false;
  // An AP client has already used the connection help to join. Prepare RAM
  // before parsing its first HTTP request. This does not grant session
  // authority: status and every owned request still check reader and token.
  // STA keeps its help/fallback until verified contact on that route.
  if (!_server.verifiedContact() && !(associatedHotspot && _directMode && WiFi.softAPgetStationNum() > 0)) {
    return false;
  }
  // HTTP polling admits only diagnostics/cancel. The outer-tick retry must
  // not recursively process general USB scene commands from this method.
  const auto cancelled = [] {
    return transfer_sync::handlingHttp() ? transfer_sync::pollControls() : transfer_sync::cancelRequested();
  };
  const auto routeUsable = [this] {
    return _directMode ? WiFi.softAPgetStationNum() > 0 : WiFi.status() == WL_CONNECTED;
  };
  if (cancelled() || !routeUsable()) return false;
  if (gWifiTestMode) return true;  // the normal SDK's route test does not need display RAM
  if (_staticSync) {
#if defined(FLOWE_SYNC_FAST_SDK)
    return _framebufferReleased;
#else
    return true;  // normal/static-buffer fallback
#endif
  }
  if (SCENES.paused()) return false;
  // Keep the current picture for phone sync. Manual File Transfer keeps its
  // connection help. Finish the status bar before releasing display RAM;
  // the e-ink picture remains on glass without the framebuffer.
  // Recheck after each flush: cancellation or route loss can arrive while
  // the panel worker runs. Never release before that worker is idle.
  SCENES.waitFlushIdle();
  if (cancelled() || !routeUsable()) return false;
  if (!_silent) {
    markDirty();
    SCENES.renderIfDirty(*G_GFX);
    SCENES.waitFlushIdle();
    if (isDirty() || cancelled() || !routeUsable()) return false;
  }
  // Refresh even if the text is unchanged: route setup can have changed
  // the network label since the first bar was drawn.
  if (!paintPill("Syncing...")) return false;
  SCENES.waitFlushIdle();
  if (cancelled() || !routeUsable()) return false;
  clearDirty();
  _staticSync = true;
  SCENES.setPaused(true);
  _server.progressHook = nullptr;
  // ReaderScene::onExit discarded its inflate/cache borrow before entry.
  const unsigned before = ESP.getFreeHeap();
  _framebufferReleased = G_GFX->releaseFramebufferForSync();
  sSyncDisplay = true;
  Serial.printf("[syncmem] static=%d released=%d bytes=%u heap=%u->%u largest=%u tcpWnd=%u tcpSnd=%u\n",
                _staticSync, _framebufferReleased, static_cast<unsigned>(G_GFX->display().getBufferSize()),
                before, ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(TCP_WND), static_cast<unsigned>(TCP_SND_BUF));
#if defined(FLOWE_SYNC_FAST_SDK)
  return _framebufferReleased;
#else
  return true;
#endif
}

void FileTransferScene::restoreSyncMemory() {
  // Only used for an in-process exit after server/radio shutdown. Normal
  // success/cancel uses the existing quiet restart and allocates at boot.
  if (_framebufferReleased && (!G_GFX || !G_GFX->restoreFramebufferAfterSync())) {
    const SceneId land = _inPlace ? static_cast<SceneId>(_returnSceneId) : SceneId::Launcher;
    Serial.println("[syncmem] restore allocation failed; quiet restart");
    quietRestartToScene(static_cast<uint32_t>(land));
  }
  _framebufferReleased = false;
  _staticSync = false;
  sSyncDisplay = false;
  if (G_GFX) G_GFX->setOrientation(Gfx::Orient::Portrait);
  SCENES.setPaused(false);
}

void FileTransferScene::onEnter() {
  _staticSync = false;
  _framebufferReleased = false;
  sSyncDisplay = false;
  sCancelSync = false;
  _state = State::Idle;
  _parkedSinceMs = millis();
  _radioWasUp = false;
  _staEventHandle = 0;
  _directMode = false;
  _targetGiven = false;
  _hotspotFallback = false;
  _targetSsid[0] = '\0';
  _targetPass[0] = '\0';
  _targetSessionCreds = false;
  _showApPassword = false;
  _shownApClients = -1;
  _failReason = "";
  _failedAtMs = 0;
  _ip[0] = '\0';
  _shownRequests = 0;
  _shownKb = 0;
  WifiCreds::load(_ssid, sizeof(_ssid), _password, sizeof(_password));
  markDirty();
}

void FileTransferScene::beginSilent(const uint32_t returnSceneId) {
  _silent = true;
  _inPlace = true;
  sSilentNow = true;
  _returnSceneId = returnSceneId;
  _silentSinceMs = millis();
  _pillShown[0] = '\0';
  clearDirty();  // switchTo marked us dirty; the previous picture stays
  paintPill("Syncing...");
}

// The sync bar (Andrew, 2026-09-07): while a sync runs in place, the
// bottom soft-key band is painted black with white text. The picture above
// it stays for the whole session. Physical BACK can cancel. In
// landscape the reader's soft-key column on the right takes the same
// treatment with the word stacked one letter per line. Flushed as a
// full-frame differential FAST refresh: only the band's pixels move.
bool FileTransferScene::paintPill(const char* text) {
  if (_staticSync || !G_GFX || !G_GFX->display().getFrameBuffer() ||
      SCENES.flushInFlight()) return false;  // try again next tick
  Gfx& gfx = *G_GFX;
  const int w = gfx.width();
  const int h = gfx.height();
  const char* place = _directMode ? "Hotspot" : (_ssid[0] ? _ssid : "");
  if (gfx.orientation() == Gfx::Orient::Landscape) {
    const int colW = Scene::SOFTKEY_BAR_H;
    const int x = w - colW;
    gfx.fillRect(x, 0, colW, h, true);
    // "SYNC", one letter per line, centred in the column.
    static constexpr const char* kWord = "SYNC";
    const int lh = gfx.lineHeight(kFontBold) + 2;
    int y = (h - 4 * lh) / 2;
    for (const char* c = kWord; *c; c++) {
      const char one[2] = {*c, '\0'};
      gfx.drawTextCentered(kFontBold, x + colW / 2, y, one, false);
      y += lh;
    }
  } else {
    const int barH = Scene::SOFTKEY_BAR_H;
    const int y0 = h - barH;
    const int kEdge = 16;
    gfx.fillRect(0, y0, w, barH, true);
    const int th = gfx.lineHeight(kFontBold);
    gfx.drawText(kFontBold, kEdge, y0 + (barH - th) / 2, text, false);
    if (place[0]) {
      const int pw = gfx.textWidth(kFontSmall, place);
      const int tw = gfx.textWidth(kFontBold, text);
      if (kEdge + tw + 24 + pw + kEdge <= w) {
        const int ph = gfx.lineHeight(kFontSmall);
        gfx.drawText(kFontSmall, w - kEdge - pw, y0 + (barH - ph) / 2, place, false);
      }
    }
  }
  gfx.flush(EInkDisplay::FAST_REFRESH);
  snprintf(_pillShown, sizeof(_pillShown), "%s", text);
  return true;
}

void FileTransferScene::silentTick() {
  if (_staticSync) return;  // the bar is on glass; networking owns its RAM
  if (isDirty()) clearDirty();  // never compose the page behind the pill
  const uint32_t now = millis();
  switch (_state) {
    case State::Idle:
      // Parked with nothing to do (no saved network: the phone got
      // "needs-wifi" and shows its own sheet). Go back where we were.
      if (now - _silentSinceMs > 1500) {
        _silent = false;
        _inPlace = false;
        sSilentNow = false;
        showSceneByIdQuiet(static_cast<SceneId>(_returnSceneId));
      }
      return;
    case State::Failed:
      if (strcmp(_pillShown, "Sync failed") != 0) paintPill("Sync failed");
      return;
    case State::Connecting:
      if (strcmp(_pillShown, "Syncing...") != 0) paintPill("Syncing...");
      return;
    case State::Running:
      if (_directMode && WiFi.softAPgetStationNum() == 0) {
        if (strcmp(_pillShown, "Waiting for your phone...") != 0) paintPill("Waiting for your phone...");
        return;
      }
      if (strcmp(_pillShown, "Syncing...") != 0) paintPill("Syncing...");
      return;
  }
}

void FileTransferScene::onExit() {
  // OS long BACK can bypass exitScene. Keep the saved return destination
  // and use the same server/radio shutdown before a quiet restart.
  if (_radioWasUp) endSession("scene exit");
  restoreSyncMemory();
  _silent = false;
  _inPlace = false;
  sSilentNow = false;
  _state = State::Idle;
}

const char* const* FileTransferScene::softKeys() const {
  static constexpr const char* kIdleKeys[4] = {"BACK", "SYNC", nullptr, nullptr};
  static constexpr const char* kIdleNoCredsKeys[4] = {"BACK", nullptr, nullptr, nullptr};
  static constexpr const char* kConnectingKeys[4] = {"CANCEL", nullptr, nullptr, nullptr};
  static constexpr const char* kRunningKeys[4] = {"EXIT", nullptr, nullptr, nullptr};
  static constexpr const char* kHotspotKeys[4] = {"EXIT", "PASSWORD", nullptr, nullptr};
  static constexpr const char* kHotspotKeysShown[4] = {"EXIT", "CODE", nullptr, nullptr};
  static constexpr const char* kFailedKeys[4] = {"BACK", "RETRY", nullptr, nullptr};
  if (_staticSync) return kConnectingKeys;
  switch (_state) {
    case State::Idle:       return _ssid[0] ? kIdleKeys : kIdleNoCredsKeys;
    case State::Connecting: return kConnectingKeys;
    case State::Running:    return !_directMode ? kRunningKeys : _showApPassword ? kHotspotKeysShown : kHotspotKeys;
    case State::Failed:     return kFailedKeys;
  }
  return kIdleKeys;
}

void FileTransferScene::autoStart() {
  // The route (2026-09-04): the phone names the network it is on. We join
  // that one only. No password for it, or the phone on no Wi-Fi at all ->
  // the hotspot at once. A phone-hotspot session arrives with its own
  // password. No "ssid" at all (older apps, the bench `sta`) -> the old
  // walk over every saved network.
  _targetGiven = COMPANION_BLE.takeTransferTarget(_targetSsid, sizeof(_targetSsid), _targetPass, sizeof(_targetPass),
                                                  &_hotspotFallback);
  _targetSessionCreds = _targetGiven && _targetPass[0] != '\0';  // decided BEFORE the store lookup below fills the field
  if (_targetGiven) {
    if (_targetPass[0] == '\0') {
      // Look the password up in the store.
      WifiCreds::Network net;
      bool found = false;
      for (int slot = 0; WifiCreds::get(slot, &net); slot++) {
        if (strcmp(net.ssid, _targetSsid) == 0) {
          snprintf(_targetPass, sizeof(_targetPass), "%s", net.pass);
          found = true;
          break;
        }
      }
      if (!found) {
        Serial.printf("[xphone-os] transfer: phone is on \"%s\", no password saved -> hotspot\n", _targetSsid);
        startAp();
        return;
      }
    }
    snprintf(_ssid, sizeof(_ssid), "%s", _targetSsid);
    snprintf(_password, sizeof(_password), "%s", _targetPass);
    if (gWifiTestMode) {
      snprintf(gWifiTestSsid, sizeof(gWifiTestSsid), "%s", _targetSsid);
      COMPANION_BLE.sendWifiTest(_targetSsid);  // the app probes for 30 s so the knock can land
      delay(300);
    }
    startSta();
    return;
  }
  WifiCreds::load(_ssid, sizeof(_ssid), _password, sizeof(_password));
  if (_ssid[0]) {
    startSta();
  } else {
    // No creds: tell the app so it can prompt for Wi-Fi instead of hanging.
    _failReason = "No Wi-Fi saved";
    _state = State::Failed;
    COMPANION_BLE.sendTransferStatus("needs-wifi");
    markDirty();
  }
}

void FileTransferScene::autoStartDirect() { startAp(); }

void FileTransferScene::restartFromCard(const bool direct) {
  switch (_state) {
    case State::Connecting:
      COMPANION_BLE.sendTransferStatus("connecting", nullptr,
                                       _directMode ? nullptr : _ssid);
      return;
    case State::Running:
      // A session is already serving; tell the phone where, so it can
      // adopt instead of hunting.
      COMPANION_BLE.sendTransferStatus("running", _ip,
                                       _directMode ? nullptr : _ssid);
      return;
    default:
      // Idle, Failed, needs-wifi: exactly as if the scene were entered
      // fresh for this card.
      onEnter();
      if (direct) autoStartDirect();
      else autoStart();
      return;
  }
}

// W2 Direct mode: no infrastructure Wi-Fi at all. The phone already holds
// the AP credentials (wifi.known "ap", minted once, BLE-encrypted); it told
// us to raise the hotspot and will join programmatically. CrossPoint ships
// this AP shape on the same chip: channel 1, max 4 clients, sleep off.
void FileTransferScene::startAp() {
  _failedAtMs = 0;
  char apSsid[33], apPass[17];
  WifiCreds::apCredentials(apSsid, sizeof(apSsid), apPass, sizeof(apPass));
  Serial.printf("[xphone-os] transfer: direct mode, raising \"%s\" (heap %u)\n", apSsid,
                ESP.getFreeHeap());
  stackProbe("transfer: before AP");
  transferMemoryProbe("transfer: before AP");
  COMPANION_BLE.sendTransferStatus("connecting", nullptr, apSsid);
  delay(600);  // one low-duty conn interval so the notify actually transmits
  COMPANION_BLE.shutdownForTransfer();
  InflateReader::releaseSharedDict();
  raiseHotspot();
}

void FileTransferScene::raiseHotspot() {
  // The bench 'isolate' lever models a guest network that blocks clients
  // from each other. Our own hotspot is not that network: answer here.
  // (Found by the power session on the X3, 2026-09-04 23:35.)
  FileTransferServer::isolate = false;
  char apSsid[33], apPass[17];
  WifiCreds::apCredentials(apSsid, sizeof(apSsid), apPass, sizeof(apPass));
  _radioWasUp = true;
  _directMode = true;
  _showApPassword = false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
#if defined(FLOWE_BENCH_AP_CHANNEL)
#if FLOWE_BENCH_AP_CHANNEL < 1 || FLOWE_BENCH_AP_CHANNEL > 11
#error "FLOWE_BENCH_AP_CHANNEL must be in 1..11"
#endif
  constexpr int apChannel = FLOWE_BENCH_AP_CHANNEL;
#else
  constexpr int apChannel = 1;
#endif
  if (!WiFi.softAP(apSsid, apPass, apChannel, /*ssid_hidden=*/0, /*max_connection=*/4)) {
    Serial.println("[xphone-os] transfer: softAP failed");
    _failReason = "Hotspot failed to start";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
#if defined(FLOWE_BENCH_AP_HT20)
  const esp_err_t bandwidthSet = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  Serial.printf("[apbench] HT20 set rc=%d\n", static_cast<int>(bandwidthSet));
#endif
  // Off the ESP default 192.168.4.x: home LANs use it too (Andrew's does),
  // so a phone-side request to the AP address could reach a ROUTER instead
  // of us. Config after softAP() — before, some cores overwrite it.
  delay(100);
  if (!WiFi.softAPConfig(IPAddress(192, 168, 44, 1), IPAddress(192, 168, 44, 1),
                         IPAddress(255, 255, 255, 0))) {
    Serial.println("[xphone-os] transfer: softAPConfig failed, staying on default subnet");
  }
  snprintf(_ssid, sizeof(_ssid), "%s", apSsid);  // the panel names the hotspot
  snprintf(_ip, sizeof(_ip), "%s", WiFi.softAPIP().toString().c_str());
  Serial.printf("[xphone-os] transfer: hotspot up, ip %s heap %u\n", _ip, ESP.getFreeHeap());
#if defined(FLOWE_BENCH_AP_CHANNEL) || defined(FLOWE_BENCH_AP_HT20)
  uint8_t actualChannel = 0;
  wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  wifi_bandwidth_t bandwidth = WIFI_BW_HT20;
  const esp_err_t channelGet = esp_wifi_get_channel(&actualChannel, &secondary);
  const esp_err_t bandwidthGet = esp_wifi_get_bandwidth(WIFI_IF_AP, &bandwidth);
  Serial.printf("[apbench] requestedChannel=%d actualChannel=%u secondary=%d channelRc=%d bandwidth=%d bandwidthRc=%d\n",
                apChannel, static_cast<unsigned>(actualChannel), static_cast<int>(secondary),
                static_cast<int>(channelGet), static_cast<int>(bandwidth), static_cast<int>(bandwidthGet));
#endif
  _apStartedMs = millis();
  _apClientLeftMs = 0;
  _state = State::Running;
  startServerOrFail();
}

void FileTransferScene::stopSession() { endSession("phone (ble)"); }

// "Test this network" (device Wi-Fi screen): record one sentence, end the
// session without a "stopped" for the phone, and return to the Wi-Fi screen.
void FileTransferScene::finishTest(const char* sentence) {
  snprintf(gWifiTestResult, sizeof(gWifiTestResult), "%s", sentence);
  gWifiTestAtMs = millis();
  Serial.printf("[xphone-os] wifi test: %s\n", sentence);
  gWifiTestMode = false;
  endSession("wifi test", /*queueStopped=*/false, /*backToWifi=*/true);
}

void FileTransferScene::switchToHotspot(const char* why) {
  if (_staticSync) { endSession(why); return; }
  Serial.printf("[xphone-os] transfer: %s -> becoming the hotspot (heap %u)\n", why, ESP.getFreeHeap());
  _server.stop();
  MDNS.end();
  if (_staEventHandle) {
    WiFi.removeEvent(_staEventHandle);
    _staEventHandle = 0;
  }
  WiFi.disconnect(/*wifioff=*/true, /*eraseap=*/false);
  WiFi.mode(WIFI_OFF);
  delay(150);
  _failedAtMs = 0;
  raiseHotspot();
  markDirty();
}

// The one way out of a session (no-restart exit, 2026-09-04). Order matters:
// the server and mDNS go first so no request is mid-flight during the
// radio teardown; BLE returns strictly AFTER Wi-Fi is off, which both apps
// rely on ("BLE up => the Wi-Fi session is over"). Runs on the main loop,
// never inside handleClient (the /stop handler only raises a flag).
void FileTransferScene::endSession(const char* reason, const bool queueStopped, const bool backToWifi) {
  Serial.printf("[xphone-os] transfer: session end (%s) heap=%u largest=%u\n", reason, ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  SCENES.waitFlushIdle();
  _server.stop();
  MDNS.end();
  const bool radioWasUp = _radioWasUp;
  if (radioWasUp) teardownRadio();
  transferSetSessionToken("", true);  // a restart used to clear it; now by hand
  // Andrew, 2026-09-07: a sync ends with a quiet restart (3 s, no splash)
  // that lands back on the screen the sync interrupted, the reader at its
  // saved page. The in-place exit stays for the failure path that must
  // show the Wi-Fi screen, and as the 0.8 goal once its memory is clean:
  // every session left 15 KB behind for the rest of the boot.
  if (radioWasUp && !backToWifi) {
    if (queueStopped) {
      Preferences p;  // "stopped" reaches the phone after the restart (main.cpp)
      if (p.begin("xfer", /*readOnly=*/false)) { p.putUChar("stopped", 1); p.end(); }
    }
    const SceneId land = _inPlace ? static_cast<SceneId>(_returnSceneId) : SceneId::Launcher;
    Serial.printf("[xphone-os] transfer: session end (%s); restarting to %lu\n", reason,
                  static_cast<unsigned long>(land));
    _silent = false;
    _inPlace = false;
    sSilentNow = false;
    quietRestartToScene(static_cast<uint32_t>(land));  // does not return
  }
  restoreSyncMemory();
  _state = State::Idle;
  _failedAtMs = 0;
  if (radioWasUp) {
    COMPANION_BLE.resumeAfterTransfer(queueStopped ? "stopped" : nullptr, reason);
    stackProbe("transfer: after BLE resume");
  transferMemoryProbe("transfer: after BLE resume");
  } else {
    // BLE never went down (parked screens): say it now.
    COMPANION_BLE.sendTransferStatus("stopped", nullptr, reason);
  }
  // Return to the screen that the phone's sync interrupted.
  const bool goBack = _inPlace;
  const SceneId back = static_cast<SceneId>(_returnSceneId);
  _silent = false;
  _inPlace = false;
  sSilentNow = false;
  if (backToWifi) showWifi();
  else if (goBack) showSceneByIdQuiet(back);
  else showLauncher();
}

// Closed connections linger in lwIP's TIME_WAIT list for two minutes,
// 256 B of heap each (measured 2026-09-07: six blocks after four console
// sessions, all freed by themselves after the wait). A phone sync serves
// dozens of requests, so it can tie up kilobytes exactly when the reader
// or the returning Bluetooth stack needs them. The radio is going off:
// nothing on those sockets can still arrive, so drop them now, on the
// lwIP thread where the lists may be touched.
static void purgeTimeWaitPcbs() {
  const uint32_t heapBefore = ESP.getFreeHeap();
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
  struct PurgeResult { unsigned count = 0; uint32_t before = 0, after = 0; } result;
  const err_t rc = tcpip_callback_wait([](void* context) {
    auto& r = *static_cast<PurgeResult*>(context);
    r.before = ESP.getFreeHeap();
    while (tcp_tw_pcbs) { ++r.count; tcp_abort(tcp_tw_pcbs); }
    r.after = ESP.getFreeHeap();
  }, &result);
  Serial.printf("[netmem-purge] rc=%d count=%u before=%lu after=%lu\n", rc, result.count,
                (unsigned long)result.before, (unsigned long)result.after);
#else
  tcpip_callback([](void*) {
    while (tcp_tw_pcbs) tcp_abort(tcp_tw_pcbs);
  }, nullptr);
#endif
  delay(30);  // let the callback run before WIFI_OFF tears the stack down
  Serial.printf("[xphone-os] transfer: TIME_WAIT purge heap %u -> %u\n", heapBefore, ESP.getFreeHeap());
}

void FileTransferScene::teardownRadio() {
  const uint32_t t0 = millis();
  purgeTimeWaitPcbs();
  if (_staEventHandle) {
    WiFi.removeEvent(_staEventHandle);  // one node per session; used to leak
    _staEventHandle = 0;
  }
  if (_directMode) {
    WiFi.softAPdisconnect(/*wifioff=*/true);
  } else {
    WiFi.disconnect(/*wifioff=*/true, /*eraseap=*/false);
  }
  // Arduino 3.x: esp_wifi_stop + netif destroy + esp_wifi_deinit; the
  // driver's static RX buffers (~13 KB) come back here.
  WiFi.mode(WIFI_OFF);
  delay(100);  // let the radio settle before NimBLE takes it
  gLastStaDisconnectReason = 0;
  _radioWasUp = false;
  _directMode = false;
  Serial.printf("[xphone-os] transfer: after WIFI_OFF heap=%u largest=%u (%lu ms)\n", ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned long>(millis() - t0));
}

void FileTransferScene::startSta() {
  _failedAtMs = 0;  // a RETRY re-arms the join; the linger clock belongs to Failed only
  // pw LENGTH only, never the text: length mismatches expose paste/truncation
  // bugs (reason=15 loops) without putting the secret on a serial port.
  // CRC-32 of the password (never the text): compared with the phone's own
  // line when a join fails the handshake (2026-09-05 03:30).
  uint32_t pwCrc = 0xFFFFFFFFu;
  for (const char* q = _password; *q; q++) {
    pwCrc ^= static_cast<uint8_t>(*q);
    for (int k = 0; k < 8; k++) pwCrc = (pwCrc >> 1) ^ (0xEDB88320u & (0u - (pwCrc & 1u)));
  }
  pwCrc ^= 0xFFFFFFFFu;
  Serial.printf("[xphone-os] transfer: password crc=%08lx\n", static_cast<unsigned long>(pwCrc));
  Serial.printf("[xphone-os] transfer: joining \"%s\" (pw len=%u, heap %u)\n", _ssid,
                static_cast<unsigned>(strlen(_password)), ESP.getFreeHeap());
  // Tell the phone what is about to happen, THEN drop BLE: the X3 idles at
  // ~39 KB free with BLE+ANCS up and esp_wifi needs ~50 KB, so the two
  // stacks cannot coexist (measured abort on WiFi.mode with BLE running).
  // The phone finds the server over mDNS/HTTP from here on.
  COMPANION_BLE.sendTransferStatus("connecting", nullptr, _ssid);
  // Give the notify real time to leave. 600 ms was one connection interval,
  // but ANCS asks for 90-180 ms with slave latency 4, so the phone may skip
  // four intervals — up to ~900 ms — before it listens at all. On
  // 2026-08-19 a session logged "Transfer status sent" and "BLE
  // authentication complete" in the same second, then shut BLE down: the
  // phone never saw the notify and waited while the device served an empty
  // network for three minutes. The phone now starts looking on its own
  // after six seconds, so this delay is belt to that braces.
  delay(1200);
  stackProbe("transfer: before BLE shutdown");
  transferMemoryProbe("transfer: before BLE shutdown");
  COMPANION_BLE.shutdownForTransfer();
  stackProbe("transfer: after BLE shutdown");
  transferMemoryProbe("transfer: after BLE shutdown");
  // If ReaderScene left a 32 KB inflate dict parked, esp_wifi needs that
  // heap back. Usually a no-op (the dict lives only while Reader is up).
  InflateReader::releaseSharedDict();

  _radioWasUp = true;
  WiFi.persistent(false);   // creds live in our NVS keys, not the SDK blob
  // Diagnosis rail: the join loop only ever sees WL_DISCONNECTED (status=6)
  // and can't say WHY association fails. The SDK's disconnect event carries
  // the 802.11 reason code (2=AUTH_EXPIRE, 15/204=4-way handshake = wrong
  // password, 201=NO_AP_FOUND — see esp_wifi_types.h wifi_err_reason_t);
  // log every one. Captureless lambda: no heap, survives the scene.
  _staEventHandle = WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        gLastStaDisconnectReason = static_cast<int>(info.wifi_sta_disconnected.reason);
        Serial.printf("[xphone-os] transfer: STA disconnect reason=%d\n",
                      static_cast<int>(info.wifi_sta_disconnected.reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  stackProbe("transfer: after WiFi.mode(STA)");
  transferMemoryProbe("transfer: after WiFi.mode(STA)");
  Serial.printf("[xphone-os] transfer: after WiFi.mode(STA) heap=%u largest=%u\n", ESP.getFreeHeap(), static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  if (gWifiTxPowerQuarterDb > 0) {
    // Bench lever (devcon txpwr): a phone lying on the device is a -28 dBm
    // link, hot enough to saturate a receiver; a lower TX power tests that.
    esp_wifi_set_max_tx_power(gWifiTxPowerQuarterDb);
    int8_t got = 0;
    esp_wifi_get_max_tx_power(&got);
    Serial.printf("[xphone-os] transfer: max TX power set to %d (0.25 dBm units)\n", got);
  }
  WiFi.setSleep(false);     // modem sleep costs throughput (CrossPoint keeps it off too)
  WiFi.setHostname(kHostname);
  // Do NOT try raising TX power for a weak link: measured 2026-08-21, it
  // already comes up at 80 (0.25 dBm units) = 20 dBm, the maximum, so
  // setTxPower(WIFI_POWER_19_5dBm) only turns it DOWN. There is no
  // headroom here and the lever does not exist.

  // W1: scan once, then try saved networks strongest-first. The scan also
  // feeds the app's honesty copy ("your device saw CafeBlue") — names are
  // persisted and reported over BLE after the session restart.
  const int found = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  char seen[384];
  size_t seenLen = 0;
  seen[0] = '\0';
  for (int i = 0; i < found && seenLen + 34 < sizeof(seen); i++) {
    const String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;  // one name per network; a mesh answers once per node
    for (int m = 0; m < i && !dup; m++) dup = WiFi.SSID(m) == s;
    if (dup) continue;
    seenLen += (size_t)snprintf(seen + seenLen, sizeof(seen) - seenLen, "%s%s",
                                seenLen ? "\n" : "", s.c_str());
  }
  WifiCreds::saveSeen(seen);
  {
    // The richer record: strongest first, one line per network. The phone
    // shows "seen nearby, strong" and the device Wi-Fi screen its bars.
    int order[24];
    int n = found < 24 ? found : 24;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) {
      int j = i;
      while (j > 0 && WiFi.RSSI(order[j - 1]) < WiFi.RSSI(order[j])) { const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; j--; }
    }
    char scan[512];
    size_t len = 0;
    scan[0] = '\0';
    for (int k = 0; k < n && len + 48 < sizeof(scan); k++) {
      const int i = order[k];
      const String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;  // one line per name; the strongest copy is first
      for (int m = 0; m < k && !dup; m++) dup = WiFi.SSID(order[m]) == s;
      if (dup) continue;
      len += (size_t)snprintf(scan + len, sizeof(scan) - len, "%s%s\t%d\t%d\t%d", len ? "\n" : "", s.c_str(),
                              WiFi.RSSI(i), static_cast<int>(WiFi.encryptionType(i)), WiFi.channel(i));
    }
    WifiCreds::saveScan(scan);
  }
  Serial.printf("[xphone-os] transfer: scan saw %d networks\n", found);
  stackProbe("transfer: after scan");
  transferMemoryProbe("transfer: after scan");

  if (_targetGiven) {
    // Only the network the phone is on. Visible -> join it. Not visible ->
    // one try anyway if it worked here before (a single scan misses real
    // networks, 2026-08-23), otherwise the hotspot at once: a 5 GHz-only
    // phone network, or a place the device has never been.
    int best = -1000, bestAuth = -1, bestChan = -1;
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) == _targetSsid && WiFi.RSSI(i) > best) {
        best = WiFi.RSSI(i);
        bestAuth = static_cast<int>(WiFi.encryptionType(i));
        bestChan = WiFi.channel(i);
      }
    }
    WiFi.scanDelete();
    char last[WifiCreds::kMaxSsid] = {0};
    WifiCreds::lastJoinedSsid(last, sizeof(last));
    if (best == -1000 && strcmp(last, _targetSsid) != 0) {
      if (!_targetSessionCreds) WifiCreds::saveFailure(_targetSsid, 201 /* NO_AP_FOUND */);  // not for session-only creds
      if (gWifiTestMode) {
        finishTest("Not in range at this scan.");
        return;
      }
      switchToHotspot("phone's network not in range");
      return;
    }
    Serial.printf("[xphone-os] transfer: joining the phone's network \"%s\" (rssi=%d dBm, auth=%d, ch=%d)\n", _targetSsid, best,
                  bestAuth, bestChan);
    _candidateCount = 0;  // no store walk: this network or the hotspot
    _candidateIdx = 0;
    const bool wpa3Ap = bestAuth == WIFI_AUTH_WPA2_WPA3_PSK || bestAuth == WIFI_AUTH_WPA3_PSK;
    if (gTransferBadPassword) {
      char wrong[64];
      snprintf(wrong, sizeof(wrong), "%sx", _password);
      Serial.println("[xphone-os] transfer: BENCH wrong password for this join");
      WiFi.begin(_ssid, wrong);
      gTransferBadPassword = false;
    } else if (wpa3Ap) {
      // A phone's hotspot is WPA2/WPA3 mixed (Android 12 local-only
      // hotspot: auth=7). The Arduino core leaves the SAE password-element
      // method unspecified; spell it out (both methods) with PMF capable,
      // the shape Espressif's own WPA3 example uses. ORDER MATTERS: set the
      // config first, then connect once. The old shape (WiFi.begin, then
      // set_config and a second esp_wifi_connect) aborted the first connect
      // and the event hook saw reason 201 NO_AP_FOUND against an AP the
      // scan had just listed at -35 dBm (B1, 2026-09-04/05).
      WiFi.begin(_ssid, _password, 0, nullptr, /*connect=*/false);
      wifi_config_t c;
      // A mixed WPA2/WPA3 AP (auth 7) gets plain WPA2-PSK: WPA3 needs PMF,
      // so a STA that is not PMF-capable never starts SAE and the AP's
      // transition mode lets it in as a WPA2 client. With SAE the C3
      // associated and then lost the 4-way handshake (reason 15) against
      // the Moto's hotspot, three times (2026-09-05 03:23). A WPA3-only
      // AP (auth 6) still gets SAE with PMF.
      const bool wpa3Only = bestAuth == WIFI_AUTH_WPA3_PSK;
      if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {
        c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        c.sta.sae_pk_mode = WPA3_SAE_PK_MODE_AUTOMATIC;
        c.sta.pmf_cfg.capable = wpa3Only;
        c.sta.pmf_cfg.required = false;
        c.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        esp_wifi_set_config(WIFI_IF_STA, &c);
      }
      esp_wifi_connect();
      Serial.printf("[xphone-os] transfer: %s AP: %s, one connect\n", wpa3Only ? "WPA3-only" : "WPA2/WPA3 mixed",
                    wpa3Only ? "SAE with PMF" : "WPA2-PSK (no PMF, so no SAE)");
    } else {
      WiFi.begin(_ssid, _password);
    }
    _connectStartMs = millis();
    _lastPollMs = 0;
    _state = State::Connecting;
    markDirty();
    return;
  }

  // Rank the store: saved networks present in the scan by RSSI, then one
  // hidden-network try (last-joined) when nothing was seen at all.
  char last[WifiCreds::kMaxSsid] = {0};
  WifiCreds::lastJoinedSsid(last, sizeof(last));
  _candidateCount = 0;
  int rssiOf[8];
  WifiCreds::Network net;
  for (int slot = 0; WifiCreds::get(slot, &net) && _candidateCount < 8; slot++) {
    int best = -1000;
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) == net.ssid && WiFi.RSSI(i) > best) best = WiFi.RSSI(i);
    }
    if (best == -1000) continue;  // not visible; hidden fallback below
    if (strcmp(net.ssid, last) == 0) best += 3;  // last-joined tie-break
    int j = _candidateCount++;
    while (j > 0 && rssiOf[j - 1] < best) {
      rssiOf[j] = rssiOf[j - 1];
      _candidateSlots[j] = _candidateSlots[j - 1];
      _candidateRssi[j] = _candidateRssi[j - 1];
      j--;
    }
    rssiOf[j] = best;
    _candidateSlots[j] = slot;
    _candidateRssi[j] = best;
  }
  if (_candidateCount == 0 && last[0] != '\0') {
    // Nothing visible: hidden APs don't show in a passive-name scan. Try
    // the last-joined network once before failing.
    for (int slot = 0; WifiCreds::get(slot, &net); slot++) {
      if (strcmp(net.ssid, last) == 0) {
        _candidateSlots[_candidateCount++] = slot;
        break;
      }
    }
  }
  WiFi.scanDelete();

  if (_candidateCount == 0) {
    Serial.println("[xphone-os] transfer: no saved network in sight");
    WifiCreds::saveFailure(_ssid[0] ? _ssid : "", 201 /* NO_AP_FOUND */);
    _failReason = "No saved Wi-Fi in range";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
  _candidateIdx = -1;
  tryNextCandidate();
}

void FileTransferScene::tryNextCandidate() {
  _candidateIdx++;
  if (_candidateIdx >= _candidateCount) {
    // Out of candidates. Remember why for the post-restart BLE report
    // (W3): the last 802.11 reason the event hook saw is the truth.
    if (!_targetSessionCreds) WifiCreds::saveFailure(_ssid, gLastStaDisconnectReason);  // the phone's own hotspot leaves no breadcrumb
    if (_targetGiven) {
      if (gWifiTestMode) {
        const int r = gLastStaDisconnectReason;
        const bool wrongPassword = r == 2 || r == 15 || r == 204 || r == 202;
        char msg[96];
        snprintf(msg, sizeof(msg), "%s", wrongPassword ? "Password did not work." : "Could not join (network gave up).");
        finishTest(msg);
        return;
      }
      // The phone's network refused us (wrong password, or it went away).
      // The breadcrumb above tells the phone the truth on the next connect;
      // the sync itself still completes over the hotspot.
      switchToHotspot("join failed");
      return;
    }
    if (_hotspotFallback) {
      // No target because the phone's OS withheld the name, and the saved
      // list did not get us anywhere. The phone is waiting and WILL follow
      // us onto the hotspot, so rescue the sync instead of failing it.
      switchToHotspot("no target, saved networks exhausted");
      return;
    }
    WiFi.disconnect(true);
    _failReason = "Could not join Wi-Fi";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
  WifiCreds::Network net;
  if (!WifiCreds::get(_candidateSlots[_candidateIdx], &net)) {
    tryNextCandidate();
    return;
  }
  snprintf(_ssid, sizeof(_ssid), "%s", net.ssid);
  snprintf(_password, sizeof(_password), "%s", net.pass);
  Serial.printf("[xphone-os] transfer: trying \"%s\" (%d of %d, pw len=%u, rssi=%d dBm)\n", _ssid,
                _candidateIdx + 1, _candidateCount, static_cast<unsigned>(strlen(_password)),
                _candidateRssi[_candidateIdx]);
  if (gTransferBadPassword) {
    // Bench lever (devcon 'wifibad'): join with a wrong password once, to
    // walk the failed-join exit without touching the saved credentials.
    char wrong[64];
    snprintf(wrong, sizeof(wrong), "%sx", _password);
    Serial.println("[xphone-os] transfer: BENCH wrong password for this join");
    WiFi.begin(_ssid, wrong);
    gTransferBadPassword = false;
  } else {
    WiFi.begin(_ssid, _password);
  }
  _connectStartMs = millis();
  _lastPollMs = 0;
  _state = State::Connecting;
  markDirty();
}

void FileTransferScene::pollConnecting() {
  const uint32_t now = millis();
  if (now - _lastPollMs < 250) return;
  _lastPollMs = now;

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    snprintf(_ip, sizeof(_ip), "%s", WiFi.localIP().toString().c_str());
    Serial.printf("[xphone-os] transfer: connected, ip %s\n", _ip);
    stackProbe("transfer: connected");
  transferMemoryProbe("transfer: connected");
    WifiCreds::markJoined(_ssid);
    _state = State::Running;
    startServerOrFail();
    return;
  }
  if (now - _connectStartMs > kStaTimeoutMs) {
    Serial.printf("[xphone-os] transfer: join timed out (status=%d)\n", status);
    WiFi.disconnect(/*wifioff=*/false);
    // W1: next saved network before giving up. When the list runs out,
    // tryNextCandidate records the failure for the post-restart BLE report
    // and arms the Failed auto-restart (BLE is down here; the phone would
    // otherwise wait for a server that never comes).
    tryNextCandidate();
  }
}

void FileTransferScene::startServerOrFail() {
#if defined(FLOWE_SYNC_FAST_SDK)
  // This local build has measurements only on X4. It must not silently
  // apply the larger global TCP defaults to an unverified X3 session.
  static_assert(FREEINK_FB_RELEASABLE, "Fast sync needs releasable display RAM");
  static_assert(TCP_WND == 17232 && TCP_SND_BUF == 17232, "Unexpected fast SDK windows");
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX4) {
    _failReason = "Use standard firmware for this reader";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
#endif
#if defined(FLOWE_BENCH_AP_NO_MDNS)
  // Memory experiment: Android's direct-join path probes numeric AP addresses.
  // Keep shared-network discovery. Verify app retry and guest flows before
  // considering this as a normal behavior change.
  if (_directMode) {
    Serial.println("[memprobe] AP mDNS skipped by bench flag");
  } else
#endif
  if (MDNS.begin(kHostname)) {
    Serial.printf("[xphone-os] transfer: mDNS http://%s.local/\n", kHostname);
  }
  // Keep the activity line moving during a multi-minute upload: the whole
  // body arrives inside one handleClient() call, so without this hook the
  // panel sits on stale numbers until the file ends. (2026-08-18)
  _server.progressHook = [] {
    if (sSilentNow || transfer_sync::active()) return;  // the pill does not change per chunk
    if (SCENES.active()) SCENES.active()->markDirty();
    SCENES.renderNow();
  };
  _server.prepareStatusContext = this;
  _server.prepareStatusHook = [](void* context) {
    return static_cast<FileTransferScene*>(context)->activateSyncMemory();
  };
  stackProbe("transfer: before server begin");
  transferMemoryProbe("transfer: before server begin");
  if (!_server.begin()) {
    _failReason = "Server failed to start";
    _state = State::Failed;
    _failedAtMs = millis();  // BLE is down here too — same stranding as the join timeout
    markDirty();
    return;
  }
  // Arm the STA idle guard from the moment we start serving, so a phone
  // that never arrives times out too.
  _lastActivityMs = millis();
  _servedSinceMs = millis();
  _lastRequestCount = 0;
  _lastMovedBytes = 0;
  COMPANION_BLE.sendTransferStatus("running", _ip, _ssid);
  markDirty();
}

void FileTransferScene::exitScene() {
  if (_radioWasUp) {
    endSession("exit key");
  } else {
    showLauncher();
  }
}

void FileTransferScene::handleInput(Input& in) {
  if (transfer_sync::cancelRequested()) { endSession("cancelled"); return; }
  if (_silent) {
    silentTick();
    if (!_silent && SCENES.active() != this) return;  // silentTick switched scenes
  }
  switch (_state) {
    case State::Idle:
      if (in.wasPressed(Btn::Back)) {
        exitScene();
        return;
      }
      if (in.wasPressed(Btn::Confirm) && _ssid[0]) startSta();
      // Unattended parking guard: this screen with no session running
      // gives up after 2 minutes and goes home. BLE is up; nothing ends.
      if (_parkedSinceMs && millis() - _parkedSinceMs > kParkedTimeoutMs) {
        Serial.println("[xphone-os] transfer: parked 2 min with no session; back to launcher");
        showLauncher();
        return;
      }
      break;

    case State::Connecting:
      if (in.wasPressed(Btn::Back)) {
        // BLE is down while joining; an Idle screen with no radio would be
        // unreachable from the phone. End the session properly instead.
        endSession("cancelled");
        return;
      }
      pollConnecting();
      break;

    case State::Running: {
      if (in.wasPressed(Btn::Back)) {
        exitScene();
        return;
      }
      for (int i = 0; i < kPumpPerTick; i++) {
        if (_directMode && !_staticSync) activateSyncMemory(/*associatedHotspot=*/true);
        if (transfer_sync::cancelRequested()) break;
        _server.handleClient();
        if (transfer_sync::cancelRequested()) break;
        if (!_server.stopRequested() && !_server.ownerTransferAborted()) activateSyncMemory();
        stackProbe(_server.lastUri());  // names the request that went deepest
        if (_server.stopRequested() || (!_directMode && _server.ownerTransferAborted())) break;
      }
      if (transfer_sync::cancelRequested()) { endSession("cancelled"); return; }
      if (_server.stopRequested()) {
        endSession("phone (http)");
        return;
      }
      if (!_directMode && _server.ownerTransferAborted()) {
        // The phone may now be on another network and cannot send /stop.
        // Restore BLE through the normal exit so it can request a new route.
        // Direct sessions stay up for the phone's paired-hotspot retry.
        endSession("STA transfer interrupted");
        return;
      }
      if (_directMode && !_silent && in.wasPressed(Btn::Confirm)) {
        _showApPassword = !_showApPassword;
        markDirty();
      }
      // Announce to the phone-hotspot gateway until it knocks.
      if (!_directMode && _targetGiven && _targetPass[0] && _server.requestCount() == 0 &&
          millis() - _lastAnnounceMs >= kAnnounceEveryMs) {
        _lastAnnounceMs = millis();
        WiFiUDP udp;
        char msg[64];
        snprintf(msg, sizeof(msg), "flowe ip=%s", _ip);
        if (udp.beginPacket(WiFi.gatewayIP(), kAnnouncePort)) {
          udp.write(reinterpret_cast<const uint8_t*>(msg), strlen(msg));
          udp.endPacket();
        }
      }
      // Reach test: served for kKnockTimeoutMs and nobody knocked. On a
      // guest network with client isolation the phone never can. Become
      // the hotspot; the phone joins it after its own budget runs out.
      if (gWifiTestMode && !_directMode) {
        char msg[96];
        if (_server.requestCount() > 0) {
          snprintf(msg, sizeof(msg), "Joined in %lu s at %s. Your phone reached it.",
                   static_cast<unsigned long>((_servedSinceMs - _connectStartMs + 500) / 1000), _ip);
          finishTest(msg);
          return;
        }
        if (millis() - _servedSinceMs > kKnockTimeoutMs) {
          snprintf(msg, sizeof(msg), "Joined at %s, but your phone could not reach it here.", _ip);
          finishTest(msg);
          return;
        }
      }
      if (!_directMode && (_targetGiven || _hotspotFallback) && !_server.verifiedContact() &&
          millis() - _servedSinceMs > kKnockTimeoutMs) {
        switchToHotspot("no knock in 20 s");
        return;
      }

      // STA link health (CrossPoint checks every 2 s; driver auto-reconnects).
      // Direct mode has no STA link to lose — WiFi.status() is never
      // WL_CONNECTED while we ARE the access point, so this check would
      // cry "dropped" every 2 s at a perfectly healthy hotspot. Report
      // client count there instead. (Found live, 2026-08-18.)
      const uint32_t now = millis();
      if (now - _lastPollMs > 2000) {
        _lastPollMs = now;
        if (_directMode) {
          const int clients = WiFi.softAPgetStationNum();
          if (clients != _shownApClients) {
            Serial.printf("[xphone-os] transfer: hotspot clients %d\n", clients);
            // Give a disconnected phone the full grace period, even after
            // a long session. Measuring from JOIN ended those sessions
            // immediately when the phone briefly left the hotspot.
            if (clients == 0 && _shownApClients > 0) _apClientLeftMs = now;
            if (clients > 0) _apClientLeftMs = 0;
            _shownApClients = clients;
            markDirty();
          }
          // The two-second poll can miss a brief join after RAM release.
          // In that case, start the reconnect grace at the observed loss.
          if (clients == 0 && _staticSync && _apClientLeftMs == 0) _apClientLeftMs = now;
          // Nobody ever joined (the user dismissed the phone's join dialog)
          // or everybody left mid-session: BLE is down, so the phone cannot
          // tell us to stop and auto-sleep is pinned. Come home by
          // ourselves rather than hold the radio up until the battery dies.
          // (Release audit, 2026-08-18.)
          if (clients == 0 && _apClientLeftMs == 0 &&
              now - _apStartedMs > kApNoClientTimeoutMs) {
            Serial.println("[xphone-os] transfer: hotspot had no client; ending the session");
            endSession("no client");
            return;
          }
          if (clients == 0 && _apClientLeftMs != 0 &&
              now - _apClientLeftMs > kApNoClientTimeoutMs) {
            Serial.println("[xphone-os] transfer: hotspot client left; ending the session");
            endSession("client left");
            return;
          }
          // A phone that joined and then went quiet (its app died mid-session,
          // 2026-09-04 23:45 on the X3) used to hold the hotspot for ever:
          // only the two client-count guards existed here. Same idle guard
          // as the shared-network path: any request or byte counts as life.
          if (clients > 0) {
            const uint32_t moved = _server.bytesUploaded() + _server.bytesDownloaded();
            if (_server.requestCount() != _lastRequestCount || moved != _lastMovedBytes) {
              _lastRequestCount = _server.requestCount();
              _lastMovedBytes = moved;
              _lastActivityMs = now;
            } else if (_lastActivityMs != 0 && now - _lastActivityMs > kStaIdleTimeoutMs) {
              Serial.println("[xphone-os] transfer: hotspot client idle for 3 min; ending the session");
              endSession("idle 3 min");
              return;
            }
          }
        } else if (WiFi.status() != WL_CONNECTED) {
          if (_staticSync) { endSession("Wi-Fi lost"); return; }
          Serial.println("[xphone-os] transfer: Wi-Fi dropped; waiting for auto-reconnect");
        } else {
          // STA idle guard. Any HTTP request counts as life. Bytes count
          // too, so one long upload never looks idle.
          const uint32_t moved = _server.bytesUploaded() + _server.bytesDownloaded();
          if (_server.requestCount() != _lastRequestCount || moved != _lastMovedBytes) {
            _lastRequestCount = _server.requestCount();
            _lastMovedBytes = moved;
            _lastActivityMs = now;
          } else if (_lastActivityMs != 0 && now - _lastActivityMs > kStaIdleTimeoutMs) {
            Serial.println("[xphone-os] transfer: no phone for 3 min; ending the session");
            endSession("idle 3 min");
            return;
          }
        }
      }

      // Activity line refresh, only on meaningful change (e-ink discipline):
      // request count moved, or another 256 KB crossed the wire.
      const uint32_t kb = (_server.bytesUploaded() + _server.bytesDownloaded()) / 1024;
      if (_server.requestCount() != _shownRequests || kb / 256 != _shownKb / 256) {
        _shownRequests = _server.requestCount();
        _shownKb = kb;
        markDirty();
      }
      break;
    }

    case State::Failed:
      if (in.wasPressed(Btn::Back)) {
        exitScene();
        return;
      }
      if (in.wasPressed(Btn::Confirm) && _ssid[0]) {
        startSta();
        return;
      }
      // Radio-down failures (join timeout / server start) can't notify the
      // phone — BLE is gone. After a readable pause, exit via the normal
      // restart path so BLE returns and the phone reconnects. _failedAtMs
      // stays 0 for needs-wifi (BLE still up), which keeps its RETRY screen.
      if (_failedAtMs == 0 && _parkedSinceMs &&
          millis() - _parkedSinceMs > kParkedTimeoutMs) {
        // The needs-wifi RETRY screen, unattended. BLE is up; just leave.
        Serial.println("[xphone-os] transfer: parked 2 min with no session; back to launcher");
        showLauncher();
        return;
      }
      if (_failedAtMs && millis() - _failedAtMs > kFailedLingerMs) {
        // The phone is waiting and cannot be told — BLE is gone. Leave the
        // reason where the reconnect can find it; CompanionBleService
        // announces it at the phone's first write. Without this the phone
        // polled a network that never came up for 75 seconds and showed
        // nothing (2026-08-22). No "stopped" is queued: it would land after
        // "failed" and wipe the message the user needs to see.
        Preferences p;
        if (p.begin("transfer", /*readOnly=*/false)) {
          p.putString("lastFail", _failReason[0] ? _failReason : "Wi-Fi failed");
          p.end();
        }
        Serial.println("[xphone-os] transfer: failed with radio down; ending the session");
        endSession("failed", /*queueStopped=*/false);
        return;
      }
      break;
  }
}

void FileTransferScene::render(Gfx& gfx) {
  gfx.drawText(kFontBold, kMarginX, 8, "File Transfer");
  gfx.fillRect(0, kHeaderH - 2, gfx.width(), 2, true);

  const int w = gfx.width();
  const int lineReg = gfx.lineHeight(kFontRegular);
  const int lineBold = gfx.lineHeight(kFontBold);
  int y = kHeaderH + 24;

  switch (_state) {
    case State::Idle:
      gfx.drawText(kFontRegular, kMarginX, y, "Move books with the Flowe app");
      y += lineReg + 4;
      gfx.drawText(kFontRegular, kMarginX, y, "over Wi-Fi.");
      y += lineReg + 24;
      if (_ssid[0]) {
        gfx.drawText(kFontBold, kMarginX, y, "Saved network");
        y += lineBold + 4;
        gfx.drawText(kFontRegular, kMarginX, y, _ssid);
        y += lineReg + 20;
        gfx.drawText(kFontRegular, kMarginX, y, "Press SYNC to join it, or start");
        y += lineReg + 4;
        gfx.drawText(kFontRegular, kMarginX, y, "a sync from the Flowe app.");
      } else {
        gfx.drawText(kFontBold, kMarginX, y, "No Wi-Fi saved yet");
        y += lineBold + 4;
        gfx.drawText(kFontRegular, kMarginX, y, "Add your network in the Flowe");
        y += lineReg + 4;
        gfx.drawText(kFontRegular, kMarginX, y, "app (Read tab > Sync).");
      }
      break;

    case State::Connecting: {
      char line[96];
      snprintf(line, sizeof(line), "%s %s...", gWifiTestMode ? "Testing" : "Joining", _ssid);
      gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - lineBold, line);
      break;
    }

    case State::Running: {
      if (_directMode) {
        drawHotspotScreen(gfx, y);
        break;
      }
      gfx.drawText(kFontBold, kMarginX, y, gWifiTestMode ? "Testing the network" : "Ready to sync");
      y += lineBold + 4;
      gfx.drawText(kFontRegular, kMarginX, y, _ssid);
      y += lineReg + 20;

      char url[40];
      snprintf(url, sizeof(url), "http://%s/", _ip);
      gfx.drawTextCentered(kFontBold, w / 2, y, url);
      y += lineBold + 6;
      gfx.drawTextCentered(kFontRegular, w / 2, y,
                           gWifiTestMode ? "Joined. Waiting for your phone to knock." : "Open Flowe > Read > Sync");
      y += lineReg + 24;

      char stats[64];
      const uint32_t kb = (_server.bytesUploaded() + _server.bytesDownloaded()) / 1024;
      snprintf(stats, sizeof(stats), "%u request%s   %lu KB moved", static_cast<unsigned>(_server.requestCount()),
               _server.requestCount() == 1 ? "" : "s", static_cast<unsigned long>(kb));
      gfx.drawTextCentered(kFontRegular, w / 2, y, stats);
      break;
    }

    case State::Failed:
      gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - 2 * lineBold, _failReason);
      gfx.drawTextCentered(kFontRegular, w / 2, gfx.height() / 2 - lineBold + 8,
                           _ssid[0] ? "RETRY to try again" : "Add Wi-Fi in the Flowe app");
      break;
  }
}

// The hotspot screen (2026-09-04): a Wi-Fi QR code both phone cameras join
// in one tap (WIFI:T:WPA;S:<name>;P:<password>;;), the network name, and
// one plain instruction. The PASSWORD key swaps the code for the text
// (Andrew, 4 Sept: the device may show it; the code stays the default).
void FileTransferScene::drawHotspotScreen(Gfx& gfx, int y) {
  const int w = gfx.width();
  const int lineReg = gfx.lineHeight(kFontRegular);
  const int lineBold = gfx.lineHeight(kFontBold);
  char apSsid[33], apPass[17];
  WifiCreds::apCredentials(apSsid, sizeof(apSsid), apPass, sizeof(apPass));

  gfx.drawText(kFontBold, kMarginX, y, "Connect directly");
  y += lineBold + 10;
  if (_showApPassword) {
    gfx.drawText(kFontRegular, kMarginX, y, "Network");
    y += lineReg + 2;
    gfx.drawText(kFontBold, kMarginX, y, apSsid);
    y += lineBold + 12;
    gfx.drawText(kFontRegular, kMarginX, y, "Password");
    y += lineReg + 2;
    gfx.drawTextScaled(kFontBold, kMarginX, y, apPass, 2);
    y += lineBold * 2 + 16;
    gfx.drawText(kFontRegular, kMarginX, y, "Join it in your phone's Wi-Fi settings,");
    y += lineReg + 4;
    gfx.drawText(kFontRegular, kMarginX, y, "then open the Flowe app.");
  } else {
    char text[96];
    snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", apSsid, apPass);
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    const bool ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 6, qrcodegen_Mask_AUTO, true);
    if (ok) {
      const int n = qrcodegen_getSize(qr);
      int px = 240 / (n + 8);  // four modules of quiet zone each side
      if (px < 3) px = 3;
      if (px > 8) px = 8;
      const int side = n * px;
      const int x0 = (w - side) / 2;
      const int y0 = y + 4 * px;
      for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
          if (qrcodegen_getModule(qr, c, r)) gfx.fillRect(x0 + c * px, y0 + r * px, px, px, true);
        }
      }
      y = y0 + side + 4 * px + 6;
    } else {
      gfx.drawTextCentered(kFontRegular, w / 2, y, "(code too long to draw)");
      y += lineReg + 8;
    }
    gfx.drawTextCentered(kFontBold, w / 2, y, apSsid);
    y += lineBold + 10;
    gfx.drawTextCentered(kFontRegular, w / 2, y, "Point your phone's camera here");
    y += lineReg + 4;
    gfx.drawTextCentered(kFontRegular, w / 2, y, "and tap Join. Then open the Flowe app.");
  }
  y += lineReg + 18;
  char stats[64];
  const int clients = WiFi.softAPgetStationNum();
  if (clients == 0) {
    snprintf(stats, sizeof(stats), "Waiting for a phone...");
  } else {
    const uint32_t kb = (_server.bytesUploaded() + _server.bytesDownloaded()) / 1024;
    snprintf(stats, sizeof(stats), "Phone connected   %lu KB moved", static_cast<unsigned long>(kb));
  }
  gfx.drawTextCentered(kFontRegular, w / 2, y, stats);
}
