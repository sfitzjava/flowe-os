#pragma once

// xphone-os R2 — File Transfer HTTP server (trimmed port of CrossPoint's
// CrossPointWebServer, x4-os src/network/CrossPointWebServer.cpp).
//
// Serves the same JSON API the Flowe iOS app already speaks:
//   GET  /             tiny plain-text status (sanity check from a browser)
//   GET  /api/status   {"device","version","ip","mode","freeHeap"}
//   GET  /api/files    ?path=/books -> [{"name","size","isDirectory","isEpub"}]
//   GET  /download     ?path=/books/x.epub -> streamed file (4–16 KiB chunks)
//   POST /upload       ?path=/books, multipart field "file" -> SD write
//   POST /upload/raw   ?path=/books&name=..., fixed-length octet-stream
//   POST /delete       ?path=/books/x.epub -> remove file
//
// Deliberately dropped from CrossPoint: HTML pages, WebSockets, WebDAV,
// captive-portal DNS, fonts/settings/OPDS handlers. The only client is the
// phone app, which needs raw JSON + bytes.
//
// Threading: everything runs on the main loop via handleClient() polling
// (FileTransferScene pumps it), so SdFat access stays main-loop-only —
// the same single-task rule as the rest of xphone-os. CrossPoint needed a
// storage mutex only because other FreeRTOS tasks share its SD card.

#include <WebServer.h>

#include <cstdint>
#include <memory>

class FileTransferServer {
 public:
  // Starts listening on port 80. Call only after Wi-Fi is up (STA got an IP
  // or softAP started). Returns false on OOM.
  bool begin();
  // Call after mounting SD and before scanning the library on normal boot.
  static bool recoverUploads();
  void stop();
  bool isRunning() const { return _running; }
  // Set by POST /stop. The handler runs INSIDE handleClient(), so it must
  // not tear the server down under itself; it answers 200 and raises this
  // flag, and the scene ends the session on its next tick (no-restart
  // exit, 2026-09-04).
  bool stopRequested() const { return _stopRequested; }
  bool verifiedContact() const { return _verifiedContact; }
  // A failed owner transfer can strand a phone that changed Wi-Fi networks.
  // The scene may end a shared-network session after handleClient returns.
  bool ownerTransferAborted() const { return _ownerTransferAborted; }
  // Bench lever (devcon 'isolate on'): accept connections but never answer,
  // the way a guest network with client isolation looks from the phone.
  // Drives the device-side reach test (no knock in 20 s -> hotspot).
  static bool isolate;

  // Pump pending HTTP work; one call handles at most one full request
  // (Arduino WebServer processes a request synchronously inside
  // handleClient, including the whole multipart upload).
  void handleClient();

  // Bytes moved since begin() — for the on-screen activity line.
  uint32_t bytesUploaded() const { return _bytesUploaded; }
  // The URI of the last request handleClient() served ("" before the first).
  // For the loop-stack probe that names the deepest request.
  const char* lastUri() const;
  uint32_t bytesDownloaded() const { return _bytesDownloaded; }
  uint16_t requestCount() const { return _requestCount; }

  // Called at upload start and every ~4 MB of body received. A whole
  // multipart upload runs inside ONE handleClient() call, so the scene's
  // normal tick can't repaint for minutes — the panel froze at "0 KB
  // moved" during a 65 MB book (2026-08-18). Plain function pointer,
  // no heap.
  void (*progressHook)() = nullptr;

  // Synchronous status barrier, called only after target/token validation
  // and before building or sending a successful status response. The hook
  // may prepare display RAM; it must not pump HTTP or tear down this server.
  bool (*prepareStatusHook)(void*) = nullptr;
  void* prepareStatusContext = nullptr;

 private:
  struct UploadState {
    // Reserve 4 KiB at startup. A tested board can use larger sequential
    // I/O after the screen releases its RAM; allocation failure keeps 4 KiB.
    static constexpr size_t kBufferSize = 4096;
    // Heap, owned by begin()/stop(): transfer mode is the emptiest heap the
    // device ever has (Wi-Fi up, BLE down). Keeping this in BSS cost 4 KB
    // of every other scene's memory (efficiency audit 2026-09-02).
    uint8_t* buffer = nullptr;
    size_t bufferCapacity = kBufferSize;
    bool bufferGrowthTried = false;
#if defined(FLOWE_BENCH_UPLOAD_BATCH_16K)
    // Upload-only DRAM. Keep the RTC scratch unchanged for every other route
    // and for the allocation-failure fallback.
    uint8_t* largeBatch = nullptr;
#endif
    size_t bufferPos = 0;
    char path[192] = {0};  // final SD path of the file being written
  char part[200] = {0};  // <path>.part; renamed to path on a clean end
    bool fileOpen = false;
    bool failed = false;
    bool ownedBySession = false;
    size_t received = 0;
  };

  void handleRoot();
  void handleStatus();
  void handleManifest();
  void handleFileList();
  void handleDownload();
  void handleDelete();
  /// Remove the other half of a book (its source, or its package) plus their
  /// sidecars, matching on the canonical book key so two phones that spelled
  /// the same title differently still count as one book. Returns the count.
  int removeBookSiblings(const char* path);
  void handleUploadData();  // multipart body callback (START/WRITE/END/ABORT)
  void handleUploadDone();  // final response after body consumed
#if defined(FLOWE_RAW_UPLOAD)
  bool resumeAuthorized();
  int startResumeFromRequest(bool initialBody);
  void handleResumeControl(unsigned action);
  void handleResumeRaw(HTTPRaw& raw);
  void sendResumeResult(int result);
  bool _resumeRawRequest = false;
  bool _resumeRawComplete = false;
  uint32_t _resumeBodyReceived = 0;
  int _resumeResult = 0;
  void handleRawUpload(HTTPRaw& raw);
  void rejectRawUpload(const char* reason, int code);
  bool _rawUploadActive = false;
  bool _rawUploadComplete = false;
  uint32_t _rawExpectedBytes = 0;
#endif
  bool flushUploadBuffer();
  uint8_t* uploadWriteBuffer();
  size_t uploadWriteCapacity() const;
  void releaseUploadBatch();
  void closeUploadFile();

#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
  struct UploadProfile {
    bool active = false;
    uint32_t callbacks[4] = {};
    uint32_t payloadMin = UINT32_MAX, payloadMax = 0;
    uint32_t payloadSizes[5] = {};  // 0, 1..511, 512..1435, 1436, >1436
    uint32_t writeCalls = 0, shortWrites = 0, closeCalls = 0, closeFailures = 0;
    uint32_t progressCalls = 0, responseCalls = 0, batchBytes = 4096;
    uint64_t callbackStageUs[4] = {};
    uint64_t payloadBytes = 0, writtenBytes = 0;
    uint64_t callbackUs = 0, sdWriteUs = 0, sdCloseSyncUs = 0;
    uint64_t progressUs = 0, responseUs = 0;
  } _uploadProfile;
  void reportUploadProfile(uint64_t handlerUs);
#endif

  // Normalized "path" query arg into dst ("/books" default). Returns false
  // (and sends a 400) when missing/invalid.
  bool tokenOk();
  bool targetReaderOk();
  bool queryPath(char* dst, size_t dstSize, bool required);

  std::unique_ptr<WebServer> _server;
  UploadState _upload;
  bool _verifiedContact = false;
  bool _running = false;
  bool _stopRequested = false;
  bool _ownerTransferAborted = false;
  uint32_t _bytesUploaded = 0;
  uint32_t _bytesDownloaded = 0;
  uint32_t _hookMark = 0;
  uint16_t _requestCount = 0;

  // The upload FsFile is kept out of the header to avoid dragging SdFat
  // types into every includer; see .cpp (file-scope, single instance).
};
