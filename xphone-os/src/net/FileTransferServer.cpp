#include "../TransferSync.h"
#include "ControlledWrite.h"
#include "DownloadRange.h"
#include "TransferSha256.h"
#include "StatusAuthorization.h"
#include "UploadPublication.h"
#include <Arduino.h>
#include "FileTransferServer.h"
#include "HttpResponseBatch.h"
#include "WifiCreds.h"
#if defined(FLOWE_RAW_UPLOAD)
#include "ResumeUploadSd.h"
#endif

#include "../StallWatch.h"
#include "../DeviceKind.h"
#if defined(FLOWE_BENCH_TRANSFER_FLUSH_BARRIER)
#include "../Scene.h"
#endif

#include <MD5Builder.h>

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "../TransferMemoryProbe.h"
#include <esp_system.h>
#include <esp_task_wdt.h>
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
#include <esp_timer.h>
#endif
extern "C" {
#include <lwip/api.h>
#include <lwip/priv/sockets_priv.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
}

#include <cstring>
#include <functional>
#include <string>

#include "../reader/ReaderSettings.h"
#include "../reader/ReadingStats.h"
#include "../reader/FbpBook.h"
#include "../scenes/AppScenes.h"  // XPHONE_VERSION
#if defined(FLOWE_RAW_UPLOAD)
static bool finishResumeBook(const char* path);
#endif

// W3 session token (wifi-experience plan P4): minted by the phone over
// encrypted BLE before each session, required on mutating endpoints.
// /upload stays open — the W4 guest page needs it; a write-only upload is
// the accepted risk, deletes are not. RAM only; a restart clears it.
static char gSessionToken[48] = {0};

// Only the encrypted BLE token exchange selects legacy readiness. HTTP
// headers cannot downgrade a strict session. Clearing a token clears mode.
static bool gOwnedReadiness = true;
void transferSetSessionToken(const char* token, bool ownedReadiness) {
  gOwnedReadiness = !token || !token[0] || ownedReadiness;
  snprintf(gSessionToken, sizeof(gSessionToken), "%s", token ? token : "");
  Serial.printf("[xphone-os] transfer: session token set (len=%u)\n",
                (unsigned)strlen(gSessionToken));
}

namespace {
// Single upload at a time (one phone, one request in flight); keeping the
// FsFile at file scope spares every includer the SdFat headers.
FsFile gUploadFile;

#if defined(FLOWE_RAW_UPLOAD)
// The pinned RequestHandler API selects raw parsing without using the
// multipart FunctionRequestHandler's upload callback on a raw body.
class RawUploadHandler final : public RequestHandler {
 public:
  using RawCallback = std::function<void(HTTPRaw&)>;
  RawUploadHandler(RawCallback raw, WebServer::THandlerFunction done)
      : _raw(raw), _done(done) {}
  bool canHandle(WebServer&, HTTPMethod method, const String& uri) override {
    return method == HTTP_POST && uri == "/upload/raw";
  }
  bool canRaw(WebServer& server, const String& uri) override {
    return canHandle(server, server.method(), uri);
  }
  bool handle(WebServer& server, HTTPMethod method, const String& uri) override {
    if (!canHandle(server, method, uri)) return false;
    _done();
    return true;
  }
  void raw(WebServer&, const String&, HTTPRaw& body) override { _raw(body); }
 private:
  RawCallback _raw;
  WebServer::THandlerFunction _done;
};
#endif

#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
// Fixed counters and timestamps only: no allocation or per-callback output.
struct UploadDuration {
  uint64_t& total;
  uint64_t* const also;
  const int64_t started = esp_timer_get_time();
  explicit UploadDuration(uint64_t& destination, uint64_t* secondary = nullptr)
      : total(destination), also(secondary) {}
  ~UploadDuration() {
    const uint64_t elapsed = static_cast<uint64_t>(esp_timer_get_time() - started);
    total += elapsed;
    if (also) *also += elapsed;
  }
};
#endif

#if defined(FLOWE_BENCH_TRANSFER_FLUSH_BARRIER)
void waitTransferDisplay(const char* phase) {
  const uint32_t started = millis();
  SCENES.waitFlushIdle();
  const uint32_t waited = millis() - started;
  if (waited) {
    Serial.printf("[transfer-barrier] phase=%s waitedMs=%lu\n", phase,
                  static_cast<unsigned long>(waited));
  }
}
#endif

// A failed response cannot be resumed on this HTTP connection. Graceful
// close can retain its unsent data for TCP retries, starving the next request.
// SO_LINGER is disabled in our SDK. Use its socket lookup to abort only this
// client's PCB under the TCP/IP lock, then let NetworkClient release the
// socket/netconn normally. tcp_abort's error callback clears conn->pcb.tcp.
// Call synchronously from the single-client server while it still owns fd;
// never save the descriptor across stop() (the OS could reuse it).
void abortFailedDownload(NetworkClient& client) {
  struct Abort { int fd; bool aborted; } state{client.fd(), false};
  if (state.fd >= 0) {
    const err_t rc = tcpip_callback_wait([](void* context) {
      auto& s = *static_cast<Abort*>(context);
      auto* socket = lwip_socket_dbg_get_socket(s.fd);
      auto* conn = socket ? socket->conn : nullptr;
      if (conn && NETCONNTYPE_GROUP(conn->type) == NETCONN_TCP &&
          conn->state == NETCONN_NONE && conn->pcb.tcp && conn->pcb.tcp->state != LISTEN) {
        tcp_abort(conn->pcb.tcp);
        s.aborted = true;
      }
    }, &state);
    Serial.printf("[xphone-os] transfer: failed download close fd=%d aborted=%u rc=%d\n",
                  state.fd, state.aborted, rc);
  }
  client.stop();
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
  transferNetworkMemoryProbe("download-aborted");
#endif
}

bool hasEpubExtension(const char* name) {
  const size_t len = strlen(name);
  if (len < 5) return false;
  const char* ext = name + len - 5;
  return strcasecmp(ext, ".epub") == 0;
}

bool isHiddenName(const char* name) { return name[0] == '.'; }

bool hasFbpExtension(const char* name) {
  const size_t len = strlen(name);
  if (len < 5) return false;
  return strcasecmp(name + len - 4, ".fbp") == 0;
}

// Honest deletes (0.6 workstream C): every removal of a book under /books is
// recorded here so the phone NEVER re-pushes a book the user deleted. One
// name per line, path relative to /books (subdir prefix kept). Hidden file:
// the shelf scanner skips dotfiles. The app clears it after acknowledging.
constexpr const char* kTombstonePath = "/books/.tombstones";

void appendTombstone(const char* relName) {
  FsFile f = SdMan.open(kTombstonePath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    Serial.printf("[xphone-os] transfer: tombstone write failed for %s\n", relName);
    return;
  }
  f.write(reinterpret_cast<const uint8_t*>(relName), strlen(relName));
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.close();
  Serial.printf("[xphone-os] transfer: tombstoned %s\n", relName);
}

// Reject query paths that could escape or touch system areas. Absolute,
// no "..", no hidden path segments (".crosspoint" etc).
bool isSafePath(const char* path) {
  if (path[0] != '/') return false;
  if (strstr(path, "..") != nullptr) return false;
  for (const char* p = path; *p; p++) {
    if (*p == '/' && *(p + 1) == '.') return false;
  }
  return true;
}
// One synchronous upload means one journal is enough. Both names are in a
// hidden namespace which neither legacy nor raw uploads can create.
constexpr const char* kUploadBackup = "/.flowe-upload-backup";
constexpr const char* kUploadJournal = "/.flowe-upload-target";
bool recoverUploadPublication() {
  if (!SdMan.exists(kUploadBackup)) {
    if (SdMan.exists(kUploadJournal)) return SdMan.remove(kUploadJournal);
    return true;
  }
  FsFile journal = SdMan.open(kUploadJournal, O_RDONLY);
  char target[192] = {};
  if (!journal || journal.size() == 0 || journal.size() >= sizeof(target)) return false;
  const size_t size = journal.size();
  const int got = journal.read(target, size);
  const bool closed = journal.close();
  if (!closed || got != int(size) || strlen(target) != size || !isSafePath(target)) return false;
  if (!flowe_upload::recover(SdMan, target, kUploadBackup)) return false;
  return SdMan.remove(kUploadJournal);
}
bool publishUpload(const char* part, const char* target) {
  if (!recoverUploadPublication()) return false;
  // Persist the target BEFORE moving the old bytes. A partial journal can
  // never accompany our only old copy. Close checks include SdFat sync.
  FsFile journal = SdMan.open(kUploadJournal, O_WRONLY | O_CREAT | O_TRUNC);
  if (!journal) return false;
  const size_t size = strlen(target);
  const bool written = journal.write(reinterpret_cast<const uint8_t*>(target), size) == size;
  const bool closed = journal.close();
  if (!written || !closed) return false;
  const bool published = flowe_upload::publish(SdMan, part, target, kUploadBackup);
  if (!SdMan.exists(kUploadBackup)) SdMan.remove(kUploadJournal);
  return published;
}
#if defined(FLOWE_RAW_UPLOAD)
flowe_resume::SdStorage gResumeStorage;
flowe_resume::Upload<flowe_resume::SdStorage, flowe_resume::Sha256> gResumeUpload(gResumeStorage);
#endif
}  // namespace

bool FileTransferServer::recoverUploads() { return recoverUploadPublication(); }

// True when no token is set (legacy phone) or the request carries it.
bool FileTransferServer::tokenOk() {
  if (gSessionToken[0] == '\0') return true;
  if (!_server->hasHeader("X-Flowe-Token")) return false;
  return _server->header("X-Flowe-Token") == gSessionToken;
}

// A phone can change networks after discovery, where the same IP may name
// another reader. This target check is independent of session authority.
// No target keeps the guest page and older phone clients compatible.
bool FileTransferServer::targetReaderOk() {
  // WebServer takes String keys; retain the 17-byte key instead of building
  // two heap-backed temporary Strings for every upload chunk.
  static const String headerName("X-Flowe-Reader-Id");
  if (headerName.length() != sizeof("X-Flowe-Reader-Id") - 1) return false;
  const String target = _server->header(headerName);
  // Match WebServer::hasHeader: missing and present-empty both mean absent.
  if (target.length() == 0) return true;
  // This device ID does not change during this boot. Cache only our ID, never
  // the request's supplied target or its authorization result. Retry failure.
  static char readerId[WifiCreds::kReaderIdSize] = {0};
  if (!readerId[0] && !WifiCreds::readerId(readerId, sizeof(readerId))) return false;
  return target == readerId;
}

bool FileTransferServer::begin() {
  _verifiedContact = false;
  _upload.bufferCapacity = UploadState::kBufferSize;
  _upload.bufferGrowthTried = false;
  transferMemoryStart();
  // SPI and sockets copy this scratch buffer; it does not need DMA. Prefer
  // RTC fast RAM so this session-long reservation leaves DRAM for Wi-Fi's
  // DMA buffers. Fall back normally if the RTC heap cannot fit it.
  if (!_upload.buffer) _upload.buffer = static_cast<uint8_t*>(
      heap_caps_malloc(UploadState::kBufferSize, MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT));
  if (!_upload.buffer) _upload.buffer = static_cast<uint8_t*>(malloc(UploadState::kBufferSize));
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
  Serial.printf("[streamprobe] scratch=%p dmaFree=%lu rtcFree=%lu\n", _upload.buffer,
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_RTCRAM));
#endif
  if (!_upload.buffer) {
    Serial.println("[xphone-os] transfer: OOM creating the upload buffer");
    return false;
  }
  _upload.bufferPos = 0;
  class ReservedUploadServer : public WebServer {
   public:
    ReservedUploadServer() : WebServer(80) {}
    bool reserveUpload() {
      _currentUpload.reset(new (std::nothrow) HTTPUpload());
#if defined(FLOWE_RAW_UPLOAD)
      _currentRaw.reset(new (std::nothrow) HTTPRaw());
      return _currentUpload != nullptr && _currentRaw != nullptr;
#else
      return _currentUpload != nullptr;
#endif
    }
  };
  auto* server = new (std::nothrow) ReservedUploadServer();
  _server.reset(server);
  if (!server || !server->reserveUpload()) {
    Serial.println("[xphone-os] transfer: OOM reserving WebServer upload storage");
    return false;
  }

  _server->on("/", HTTP_GET, [this] { handleRoot(); });
  _server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  _server->on("/api/files", HTTP_GET, [this] { handleFileList(); });
  _server->on("/api/manifest", HTTP_GET, [this] { handleManifest(); });
  _server->on("/tombstones/clear", HTTP_POST, [this] {
    _requestCount++;
    if (!tokenOk()) { _server->send(403, "text/plain", "Missing session token"); return; }
    SdMan.remove(kTombstonePath);
    _server->send(200, "text/plain", "Cleared");
  });
  _server->on("/download", HTTP_GET, [this] { handleDownload(); });
  _server->on("/delete", HTTP_POST, [this] { handleDelete(); });
  _server->on("/stats", HTTP_GET, [this] {
    _requestCount++;
    // A populated report exceeds 4 KB. Growing its string to 8 KB crashed
    // a hotspot retry with fragmented heap. Stream the resident records.
    // Like the shelf listing, stop on the first refused socket write.
    NetworkClient client = _server->client();
    client.setNoDelay(true);
    // Reuse the upload reservation, as in handleManifest. Avoid a socket
    // write for every small statistics fragment.
    HttpResponseBatch response(_upload.buffer, _server->version() != "HTTP/1.0",
        [](void* context, const uint8_t* bytes, size_t length) {
          return static_cast<NetworkClient*>(context)->write(bytes, length);
        }, &client);
    _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server->send(200, "application/json", "");
    bool complete = reader::ReadingStats::writeJson(
        [](const char* bytes, size_t length, void* context) {
          return static_cast<HttpResponseBatch*>(context)->append(bytes, length);
        }, &response);
    if (complete) complete = response.flush();
    if (complete) _server->sendContent("");
    else client.stop();
  });
  _server->on("/health", HTTP_GET, [this] {
    // Device memory health for support reports (same numbers as the 60 s
    // serial stats line, reachable without a serial cable). BLE is always
    // down during a transfer session, so there is no "mode" field here.
    const uint32_t heapFree = ESP.getFreeHeap();
    const uint32_t largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const unsigned fragPct = heapFree ? static_cast<unsigned>(100u - (largest * 100u) / heapFree) : 0;
    char json[224];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"device\":\"%s\",\"uptimeMs\":%lu,"
             "\"heapFree\":%lu,\"heapMinFree\":%lu,\"largestBlock\":%lu,\"fragPct\":%u}",
             XPHONE_VERSION, BoardConfig::ACTIVE.name, static_cast<unsigned long>(millis()),
             static_cast<unsigned long>(heapFree), static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned long>(largest), fragPct);
    _server->send(200, "application/json", json);
  });
  _server->on(
      "/upload", HTTP_POST, [this] { handleUploadDone(); }, [this] { handleUploadData(); });
#if defined(FLOWE_RAW_UPLOAD)
  gResumeStorage.configure(_upload.buffer, _upload.bufferCapacity);
  gResumeStorage.recoverPublication = recoverUploadPublication;
  gResumeStorage.publish = publishUpload;
  gResumeStorage.publicationComplete = finishResumeBook;
  _server->on("/api/upload-resume/start", HTTP_POST, [this] { handleResumeControl(0); });
  _server->on("/api/upload-resume/status", HTTP_GET, [this] { handleResumeControl(1); });
  _server->on("/api/upload-resume/cancel", HTTP_POST, [this] { handleResumeControl(2); });
  _server->on("/api/upload-resume/reset", HTTP_POST, [this] { handleResumeControl(3); });
  auto* rawHandler = new (std::nothrow) RawUploadHandler(
      [this](HTTPRaw& raw) { handleRawUpload(raw); },
      [this] {
        if (_resumeRawRequest) {
          if (_resumeRawComplete) sendResumeResult(_resumeResult);
          else _server->send(400, "application/json", "{\"error\":\"Incomplete resume body\"}");
          _resumeRawRequest = _resumeRawComplete = false;
          return;
        }
        // A multipart body sent to this route must never reuse a
        // previous upload result or claim that it wrote a file.
        if (!_rawUploadComplete) {
          _server->send(400, "text/plain", "Expected one raw upload");
          return;
        }
        _rawUploadComplete = false;
        handleUploadDone();
      });
  if (!rawHandler) {
    Serial.println("[xphone-os] transfer: OOM reserving raw handler");
    return false;
  }
  _server->addHandler(rawHandler);
  Serial.printf("[raw-upload] reservedBytes=%u maxBodyBytes=67108864\n", static_cast<unsigned>(sizeof(HTTPRaw)));
#endif
  // End-of-session from the phone. BLE is down for the whole Wi-Fi session,
  // so "stop" must arrive over HTTP. Until 2026-09-04 this handler called
  // esp_restart() directly; now it only raises stopRequested() and the
  // scene tears Wi-Fi down and brings BLE back without a reboot.
  _server->on("/stop", HTTP_POST, [this] {
    if (!tokenOk()) {
      Serial.println("[xphone-os] transfer: stop REJECTED (token mismatch)");
      _server->send(403, "text/plain", "Missing session token");
      return;
    }
    // Tell the phone how long a normal return takes, so its "handing
    // off" state knows when to worry: Wi-Fi off ~0.1 s, BLE up ~1 s,
    // then the phone's own reconnect (3-5 s on the bench, 2026-09-04).
    _server->send(200, "application/json", "{\"state\":\"stopping\",\"bleBackMs\":2000}");
    _server->client().flush();
    Serial.println("[xphone-os] transfer: stop via HTTP");
    _stopRequested = true;
  });
  _server->onNotFound([this] { _server->send(404, "text/plain", "Not found"); });

  {
#if defined(FLOWE_RAW_UPLOAD)
    const char* headerKeys[] = {"X-Flowe-Token", "X-Flowe-Reader-Id", "Content-Type",
      "X-Flowe-Resume-Id", "X-Flowe-Resume-Secret", "X-Flowe-Resume-Offset",
      "X-Flowe-Resume-Size", "X-Flowe-Resume-SHA256", "X-Flowe-Resume-Reset",
      "Range", "If-Range", "X-Flowe-Download-Resume"};
#else
    const char* headerKeys[] = {"X-Flowe-Token", "X-Flowe-Reader-Id", "Range", "If-Range", "X-Flowe-Download-Resume"};
#endif
    _server->collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));
  }
  _server->addMiddleware([this](WebServer& server, Middleware::Callback next) {
    if (server.uri() == "/api/status" || targetReaderOk()) {
#if defined(FLOWE_SYNC_FAST_SDK)
      // Large TCP windows need the released display RAM. Discovery and stop
      // remain usable before activation; payload routes require that boundary.
      if (!transfer_sync::memoryReleased() &&
          server.uri() != "/api/status" && server.uri() != "/stop" && server.uri() != "/") {
        server.send(503, "text/plain", "Get reader status before file transfer");
        return true;
      }
#endif
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
      Serial.printf("[netmem-request] %s\n", server.uri().c_str());
      const String uri = server.uri();
      allocationProbePhase(uri == "/stats" ? 4 : uri == "/api/files" ? 3 :
                           uri == "/download" ? 5 : uri == "/upload" ? 6 :
                           uri == "/api/manifest" ? 7 : uri == "/stop" ? 8 : 2);
      transferNetworkMemoryProbe("request-before");
      const bool result = next();
      transferNetworkMemoryProbe("request-after");
      allocationProbePhase(0);
      return result;
#else
      return next();
#endif
    }
    server.send(409, "application/json", "{\"error\":\"reader-mismatch\"}");
    return true;
  });
  _server->begin();
  _running = true;
  _stopRequested = false;
  _ownerTransferAborted = false;
  _bytesUploaded = 0;
  _bytesDownloaded = 0;
  _requestCount = 0;
  // A .part left by a reset mid-upload has no owner. Sweep them at every
  // session start so the card never carries a stub for long.
  if (SdMan.ready() || SdMan.begin()) {
    if (!recoverUploadPublication())
      Serial.println("[xphone-os] transfer: upload recovery pending; prior bytes retained");
    FsFile dir = SdMan.open("/books", O_RDONLY);
    if (dir && dir.isDir()) {
      FsFile f;
      char name[160];
      int swept = 0;
      while (f.openNext(&dir, O_RDONLY)) {
        const size_t n = f.getName(name, sizeof(name));
        f.close();
        if (n >= 5 && n < sizeof(name) && strcasecmp(name + n - 5, ".part") == 0) {
          char path[200];
          snprintf(path, sizeof(path), "/books/%s", name);
          if (SdMan.remove(path)) swept++;
        }
      }
      dir.close();
      if (swept) Serial.printf("[xphone-os] transfer: swept %d stale .part file(s)\n", swept);
    }
  }
  Serial.printf("[xphone-os] transfer: HTTP server up, free heap %u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  transferMemoryProbe("server-ready");
  return true;
}

void FileTransferServer::stop() {
  if (gUploadFile) closeUploadFile();
  releaseUploadBatch();
#if defined(FLOWE_RAW_UPLOAD)
  gResumeUpload.abort();
  gResumeStorage.configure(nullptr, 0);
  _resumeRawRequest = _resumeRawComplete = false;
  _rawUploadActive = false;
  _rawUploadComplete = false;
  _rawExpectedBytes = 0;
#endif
  if (_upload.buffer) {
    free(_upload.buffer);
    _upload.buffer = nullptr;
    _upload.bufferPos = 0;
  }
  if (_server) {
    _server->stop();
    _server.reset();
  }
  _running = false;
  transferMemoryStop();
}

bool FileTransferServer::isolate = false;

void FileTransferServer::handleClient() {
  if (isolate) return;  // bench: a network where nobody can reach us
  if (_running && _server) {
    // Preserve the small startup reservation. Grow only after the screen
    // buffer has been released, before entering any HTTP handler. Failure
    // retains the working 4 KiB path; this allocation is tried once/session.
    // X4 bench: larger sequential I/O improves both directions. Keep X3
    // at its measured 4 KiB setting until its memory budget is tested.
    if (!_upload.bufferGrowthTried && transfer_sync::memoryReleased() &&
        BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4) {
      _upload.bufferGrowthTried = true;
      constexpr size_t kTransferBufferBytes = 16384;
      uint8_t* larger = static_cast<uint8_t*>(malloc(kTransferBufferBytes));
      if (larger) {
        free(_upload.buffer);
        _upload.buffer = larger;
        _upload.bufferCapacity = kTransferBufferBytes;
      }
      Serial.printf("[xphone-os] transfer: scratch=%u growth=%s heap=%u\n",
                    static_cast<unsigned>(_upload.bufferCapacity), larger ? "ok" : "fallback", ESP.getFreeHeap());
    }
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
    const int64_t started = esp_timer_get_time();
#endif
    transfer_sync::setHandlingHttp(true);
    _server->handleClient();
    transfer_sync::setHandlingHttp(false);
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
    if (_uploadProfile.active) {
      reportUploadProfile(static_cast<uint64_t>(esp_timer_get_time() - started));
      _uploadProfile.active = false;
    }
#endif
  }
}

const char* FileTransferServer::lastUri() const {
  return (_running && _server) ? _server->uri().c_str() : "";
}

bool FileTransferServer::queryPath(char* dst, const size_t dstSize, const bool required) {
  if (!_server->hasArg("path")) {
    if (required) {
      _server->send(400, "text/plain", "Missing path");
      return false;
    }
    snprintf(dst, dstSize, "/books");
    return true;
  }
  const String& arg = _server->arg("path");
  if (arg.length() == 0 || arg.length() >= dstSize) {
    _server->send(400, "text/plain", "Invalid path");
    return false;
  }
  if (arg.startsWith("/")) {
    snprintf(dst, dstSize, "%s", arg.c_str());
  } else {
    snprintf(dst, dstSize, "/%s", arg.c_str());
  }
  // Trim trailing slash (not root).
  size_t len = strlen(dst);
  while (len > 1 && dst[len - 1] == '/') dst[--len] = '\0';
  if (!isSafePath(dst)) {
    _server->send(403, "text/plain", "Forbidden path");
    return false;
  }
  return true;
}

// W4 guest page (wifi-experience plan P5): one self-contained mobile page.
// Anyone on the session's network can look and drop a book on; deletes stay
// token-gated (W3). No frameworks, no external assets.
static const char kGuestPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flowe bookshelf</title>
<style>
body{font:16px/1.5 Georgia,serif;background:#F2F1EF;color:#2C2C29;margin:0;padding:24px}
h1{font-size:22px;font-weight:600;margin:0 0 4px}
p.sub{color:#3C3C43;opacity:.6;margin:0 0 20px;font-size:14px}
#drop{border:2px dashed #769071;border-radius:12px;padding:28px;text-align:center;color:#5F7A5A;background:#FAF9F7;margin-bottom:20px}
#drop.hot{background:#EAF0E8}
#bar{height:4px;background:#DDD;border-radius:2px;margin:12px 0;display:none}
#fill{height:100%;width:0;background:#769071;border-radius:2px}
ul{list-style:none;padding:0;margin:0}
li{background:#FAF9F7;border:1px solid rgba(0,0,0,.08);border-radius:10px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;font-size:15px}
li span.sz{color:#3C3C43;opacity:.55;font-size:13px}
#msg{color:#5F7A5A;font-size:14px;min-height:20px}
input[type=file]{display:none}
label{color:#769071;font-weight:600;cursor:pointer}
</style></head><body>
<h1>Flowe bookshelf</h1>
<p class="sub">Drop an EPUB here and it lands on the device. Use the Flowe app for everything else.</p>
<div id="drop">Drop a book here or <label for="f">choose a file</label>
<input id="f" type="file" accept=".epub" multiple></div>
<div id="bar"><div id="fill"></div></div>
<div id="msg"></div>
<ul id="shelf"></ul>
<script>
function fmt(n){return n>1048576?(n/1048576).toFixed(1)+' MB':Math.round(n/1024)+' KB'}
function load(){fetch('/api/files?path=/books').then(function(r){return r.json()}).then(function(d){
 var ul=document.getElementById('shelf');ul.innerHTML='';
 (Array.isArray(d)?d:(d.files||[])).filter(function(f){return !f.dir&&f.name[0]!=='.'}).forEach(function(f){
  var li=document.createElement('li');
  var n=document.createElement('span');n.textContent=f.name;
  var s=document.createElement('span');s.className='sz';s.textContent=fmt(f.size||0);
  li.appendChild(n);li.appendChild(s);ul.appendChild(li);});});}
function send(files){var i=0;function next(){if(i>=files.length){load();return}
 var f=files[i++];var fd=new FormData();fd.append('file',f,f.name);
 var x=new XMLHttpRequest();x.open('POST','/upload?path=/books');
 document.getElementById('bar').style.display='block';
 x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(100*e.loaded/e.total)+'%'};
 x.onload=function(){document.getElementById('msg').textContent=x.status<300?f.name+' uploaded':'Upload failed: '+x.responseText;
  document.getElementById('bar').style.display='none';next()};
 x.onerror=function(){document.getElementById('msg').textContent='Upload failed';next()};
 x.send(fd)}next()}
var drop=document.getElementById('drop');
drop.addEventListener('dragover',function(e){e.preventDefault();drop.className='hot'});
drop.addEventListener('dragleave',function(){drop.className=''});
drop.addEventListener('drop',function(e){e.preventDefault();drop.className='';send(e.dataTransfer.files)});
document.getElementById('f').addEventListener('change',function(e){send(e.target.files)});
load();
</script></body></html>)HTML";

void FileTransferServer::handleRoot() {
  _requestCount++;
  // W4: the guest page. A phone-less friend on the same network (or the
  // hotspot) can see the shelf and drop a book on.
  _server->send_P(200, "text/html", kGuestPage);
}

void FileTransferServer::handleStatus() {
  _requestCount++;
  const String suppliedToken = _server->header("X-Flowe-Token");
  const String suppliedReader = _server->header("X-Flowe-Reader-Id");
  char readerId[WifiCreds::kReaderIdSize] = {};
  const bool haveReaderId = WifiCreds::readerId(readerId, sizeof(readerId));
  const auto authorization = flowe_status::authorize(
      {suppliedToken.c_str(), suppliedToken.length()}, {suppliedReader.c_str(), suppliedReader.length()},
      {gSessionToken, strlen(gSessionToken)}, {readerId, haveReaderId ? strlen(readerId) : 0}, gOwnedReadiness);
  if (!authorization.allowed) {
    _server->send(401, "application/json", "{\"error\":\"status-authorization-required\"}");
    return;
  }
  const bool verified = authorization.prepareMemory;
  if (verified) {
    _verifiedContact = true;
    // The owner starts its next request as soon as it sees status200.
    // Prepare RAM before allocating or sending that ready response.
    if (!prepareStatusHook || !prepareStatusHook(prepareStatusContext)) {
      _server->send(503, "text/plain", "Reader not ready; retry status");
      return;
    }
#if defined(FLOWE_SYNC_FAST_SDK)
    if (!transfer_sync::memoryReleased()) {
      _server->send(503, "text/plain", "Reader sync memory unavailable");
      return;
    }
#endif
  }
  // Public discovery on an owned server does not prepare payload RAM.
  // Guest/bench status retains its legacy preparation but is never paired
  // readiness: only sessionVerified grants that meaning to the client.
  JsonDocument doc;
  doc["sessionVerified"] = authorization.sessionVerified;
  if (haveReaderId) doc["readerId"] = readerId;
  doc["device"] = deviceKindUpper();
  doc["version"] = XPHONE_VERSION;
  if (verified) doc["downloadRangeVersion"] = 1;
#if defined(FLOWE_RAW_UPLOAD)
  if (verified) {
    doc["rawUploadVersion"] = 1;
    if (gSessionToken[0]) doc["uploadResumeVersion"] = 1;
    doc["compactPageVersion"] = 1;  // FBPK9 / min_reader6 / codec2
  }
#endif
  // Longest the main loop has been stuck this run. A healthy device reports
  // 0; anything here means the card, or something on it, is slow enough for
  // the owner to notice — which is the question their report starts with.
  doc["worstStallMs"] = stallwatch::worstStallMs();
  doc["gitRev"] = XPHONE_GIT_REV_STR;  // exact build for bug reports
  // In AP (Direct) mode localIP() is the STA side — 0.0.0.0. The phone
  // locks the whole session onto this address, so reporting 0.0.0.0 sent
  // every follow-up request (including /stop) to nowhere. (2026-08-18)
  const bool apMode = (WiFi.getMode() & WIFI_AP) != 0;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["freeHeap"] = ESP.getFreeHeap();
  char out[320];
  serializeJson(doc, out, sizeof(out));
  _server->send(200, "application/json", out);
}

void FileTransferServer::handleFileList() {
  _requestCount++;
  char path[192];
  if (!queryPath(path, sizeof(path), /*required=*/false)) return;

  FsFile dir = SdMan.open(path, O_RDONLY);
  if (!dir || !dir.isDir()) {
    // An empty shelf, not an error — the phone treats [] as "no books yet"
    // (e.g. /books does not exist until the first upload).
    _server->send(200, "application/json", "[]");
    return;
  }

  // Streamed (chunked) response, same shape as CrossPoint handleFileListData
  // (x4-os CrossPointWebServer.cpp:440-488) so the iOS decoder is shared.
  //
  // One chunk per ~1.4 KB batch, written straight to the client, and the
  // listing STOPS at the first write the client does not take. The old
  // shape (three small writes per entry through sendContent, no check) is
  // the freeze of 2026-09-07: a JTAG halt of a frozen X4 showed the loop in
  // NetworkClient::write -> select for one 81-byte entry. Every write that
  // the socket refuses burns the framework's ten 1 s retries, and the loop
  // then moved on to the next entry and burned ten more — 62 entries, ten
  // minutes, a device that looks dead. Now the worst case is one refused
  // batch (about 10 s), a log line, and the connection dropped.
  NetworkClient client = _server->client();
  client.setNoDelay(true);  // small chunks must not wait for the peer's delayed ACK
  _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server->send(200, "application/json", "");

  char batch[1400];
  size_t used = 0;
  unsigned entries = 0;
  bool alive = true;
  auto flushBatch = [&]() -> bool {
    if (used == 0) return true;
    char frame[sizeof(batch) + 16];
    const int head = snprintf(frame, sizeof(frame), "%x\r\n", static_cast<unsigned>(used));
    memcpy(frame + head, batch, used);
    memcpy(frame + head + used, "\r\n", 2);
    const size_t total = static_cast<size_t>(head) + used + 2;
    const size_t wrote = client.write(reinterpret_cast<const uint8_t*>(frame), total);
    used = 0;
    if (wrote != total) {
      Serial.printf("[xphone-os] transfer: listing: client stopped taking bytes after %u entries (%u of %u written); dropping it\n",
                    entries, static_cast<unsigned>(wrote), static_cast<unsigned>(total));
      client.stop();
      return false;
    }
    return true;
  };
  auto append = [&](const char* text, size_t len) -> bool {
    if (used + len > sizeof(batch) && !flushBatch()) return false;
    if (len > sizeof(batch)) return false;  // never: entries are < 256 B
    memcpy(batch + used, text, len);
    used += len;
    return true;
  };

  alive = append("[", 1);
  bool first = true;
  FsFile f;
  JsonDocument doc;
  char name[128];
  char out[256];
  while (alive && f.openNext(&dir, O_RDONLY)) {
    const size_t got = f.getName(name, sizeof(name));
    // A package the card cannot read is not a book (2026-09-07): one with a
    // bad magic on Andrew's X3 stalled the loop 21 s and cut the stream when
    // a phone copied it, which failed the phone's whole sync as "network
    // connection was lost". Four bytes per package to keep it off the list.
    bool unreadable = false;
    if (got > 0 && !f.isDir() && got > 4 && strcasecmp(name + got - 4, ".fbp") == 0) {
      uint8_t magic[4] = {0};
      if (f.read(magic, 4) != 4 || memcmp(magic, "FBPK", 4) != 0) {
        unreadable = true;
        Serial.printf("[xphone-os] transfer: listing: skipping unreadable package '%s'\n", name);
      }
    }
    if (got > 0 && got < sizeof(name) && !isHiddenName(name) && !unreadable) {
      doc.clear();
      doc["name"] = name;
      doc["size"] = f.isDir() ? 0 : static_cast<uint32_t>(f.fileSize());
      doc["isDirectory"] = f.isDir();
      doc["isEpub"] = !f.isDir() && hasEpubExtension(name);
      const size_t written = serializeJson(doc, out, sizeof(out));
      if (written < sizeof(out)) {
        if (!first) alive = append(",", 1);
        first = false;
        if (alive) alive = append(out, written);
        ++entries;
      }
    }
    f.close();
    yield();
  }
  dir.close();
  if (alive) alive = append("]", 1) && flushBatch();
  if (alive) _server->sendContent("");  // the chunked terminator, one write
  stallwatch::stage("files: done");
}

void FileTransferServer::handleDownload() {
  _requestCount++;
  // Middleware already checks identity, but its legacy String comparison
  // permits a matching prefix before NUL. Downloads require the exact
  // 12-byte supplied identity before opening any source or hashing bytes.
  const String requestedReader = _server->header("X-Flowe-Reader-Id");
  if (requestedReader.length() && (requestedReader.length() != 12 || !targetReaderOk())) {
    _server->send(409, "text/plain", "Reader identity mismatched");
    return;
  }
  char path[192];
  if (!queryPath(path, sizeof(path), /*required=*/true)) return;
#if defined(FLOWE_BENCH_TRANSFER_FLUSH_BARRIER)
  // Finish the prior display transfer before opening/reading the file or
  // sending its response. The ordinary main loop and repaints stay active.
  waitTransferDisplay("download-start");
#endif

  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) {
    _server->send(404, "text/plain", "Not found");
    return;
  }
  if (file.isDir()) {
    file.close();
    _server->send(400, "text/plain", "Path is a directory");
    return;
  }
  // A package with a bad magic stalls the loop for 21 s and cuts the stream
  // partway (2026-09-07); refuse it in one round trip instead.
  {
    const size_t plen = strlen(path);
    if (plen > 4 && strcasecmp(path + plen - 4, ".fbp") == 0) {
      uint8_t magic[4] = {0};
      const bool bad = file.read(magic, 4) != 4 || memcmp(magic, "FBPK", 4) != 0;
      file.seekSet(0);
      if (bad) {
        file.close();
        Serial.printf("[xphone-os] transfer: download refused, unreadable package '%s'\n", path);
        _server->send(409, "text/plain", "Package unreadable on the card");
        return;
      }
    }
  }

  const char* slash = strrchr(path, '/');
  const char* filename = slash ? slash + 1 : path;
  char disposition[224];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);

  const uint64_t sourceBytes = file.fileSize();
  if (sourceBytes > SIZE_MAX) {
    file.close();
    _server->send(413, "text/plain", "File exceeds response size limit");
    return;
  }
  uint8_t* const buffer = _upload.buffer;
#if defined(FLOWE_BENCH_DOWNLOAD_CHUNK)
  constexpr size_t kBufSize = FLOWE_BENCH_DOWNLOAD_CHUNK;
  static_assert(kBufSize > 0 && kBufSize <= UploadState::kBufferSize);
#else
  const size_t kBufSize = _upload.bufferCapacity;
#endif
  const String range = _server->header("Range");
  const String ifRange = _server->header("If-Range");
  const String resumeOption = _server->header("X-Flowe-Download-Resume");
  if (resumeOption.length() && resumeOption != "1") {
    file.close();
    _server->send(400, "text/plain", "Invalid download resume option");
    return;
  }
  flowe_download::Plan selected{200, 0, sourceBytes, sourceBytes};
  uint32_t hashElapsedMs = 0;
  // Legacy cover/source fanout retains its single SD pass. A new client
  // opts in on its first full download, then saves this strong ETag for
  // Range + If-Range retries. Range itself also selects this safe path.
  if (resumeOption.length() || range.length() || ifRange.length()) {
    char etag[67];
    flowe_resume::Sha256 hash;
    const uint32_t hashStartedAt = millis();
    const auto hashed = flowe_download::makeETag(file, hash, sourceBytes, buffer, kBufSize, etag,
        transfer_sync::pollControls, [] { esp_task_wdt_reset(); yield(); });
    if (hashed != flowe_download::HashResult::ok) {
      file.close();
      _server->send(hashed == flowe_download::HashResult::cancelled ? 503 : 500, "text/plain",
                    hashed == flowe_download::HashResult::cancelled ? "Download cancelled" : "Cannot verify download source");
      return;
    }
    hashElapsedMs = millis() - hashStartedAt;
    selected = flowe_download::plan(sourceBytes,
        {range.c_str(), range.length()}, {ifRange.c_str(), ifRange.length()}, etag);
    _server->sendHeader("Accept-Ranges", "bytes");
    _server->sendHeader("ETag", etag);
    _server->sendHeader("Cache-Control", "no-transform");
  }
  char contentRange[96];
  if (selected.status == 416) {
    snprintf(contentRange, sizeof(contentRange), "bytes */%llu", static_cast<unsigned long long>(sourceBytes));
    _server->sendHeader("Content-Range", contentRange);
    _server->setContentLength(0);
    _server->send(416, "application/octet-stream", "");
    file.close();
    return;
  }
  if (!file.seekSet(selected.first)) {
    file.close();
    _server->send(500, "text/plain", "Cannot seek download source");
    return;
  }
  if (selected.status == 206) {
    snprintf(contentRange, sizeof(contentRange), "bytes %llu-%llu/%llu",
        static_cast<unsigned long long>(selected.first),
        static_cast<unsigned long long>(selected.first + selected.length - 1),
        static_cast<unsigned long long>(sourceBytes));
    _server->sendHeader("Content-Range", contentRange);
  }
  const size_t expectedBytes = size_t(selected.length);
  uint64_t remainingBytes = selected.length;
  const bool ownedBySession = tokenOk();
  size_t sentBytes = 0;
  size_t nextProgress = 1024 * 1024;
  const uint32_t startedAt = millis();
  uint32_t longestReadMs = 0, longestWriteMs = 0;
  const char* failure = "short file";
  Serial.printf("[xphone-os] transfer: download start '%s' (%u bytes) status=%u offset=%lu hashMs=%lu\n",
                path, (unsigned)expectedBytes, selected.status, static_cast<unsigned long>(selected.first),
                static_cast<unsigned long>(hashElapsedMs));
  _server->setContentLength(expectedBytes);
  _server->sendHeader("Content-Disposition", disposition);
  _server->send(selected.status, hasEpubExtension(path) ? "application/epub+zip" : "application/octet-stream", "");

  // 4 KB chunked streaming, CrossPoint handleDownload pattern
  // (x4-os CrossPointWebServer.cpp:548-569). Reuses the upload batch buffer:
  // the HTTP server is synchronous single-client, so an upload body and a
  // download stream can never be in flight together — a second static 4 KB
  // here was pure BSS duplication.
  NetworkClient client = _server->client();
#if defined(FLOWE_BENCH_DOWNLOAD_COALESCE)
  // Bench-only: allow TCP to coalesce short segments during the bulk body.
  // Keep the application write size and all other routes unchanged.
  const int coalesceRc = client.setNoDelay(false);
  Serial.printf("[download-coalesce] setNoDelay(false) rc=%d getNoDelay=%d\n",
                coalesceRc, client.getNoDelay() ? 1 : 0);
#endif
  bool ok = true;
  while (ok && remainingBytes) {
    if (transfer_sync::pollControls()) { failure = "cancelled"; ok = false; break; }
    const uint32_t readAt = millis();
    const int result = flowe_download::readBounded(file, buffer, kBufSize, remainingBytes);
    longestReadMs = max(longestReadMs, millis() - readAt);
    if (result <= 0) { failure = "SD read"; break; }
    size_t sent = 0;
    while (sent < static_cast<size_t>(result)) {
      esp_task_wdt_reset();
      const uint32_t writeAt = millis();
      const auto write = transfer_sync::writeControlled(buffer + sent, result - sent, client.getTimeout(),
          [&client](const uint8_t* bytes, size_t count) {
            if (client.fd() < 0) return -1;
            const int n = ::send(client.fd(), bytes, count, MSG_DONTWAIT);
            if (n > 0) return n;
            return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
          }, [] { return millis(); }, [](unsigned ms) { delay(ms); }, transfer_sync::pollControls);
      const size_t wrote = write.bytes;
      const uint32_t writeMs = millis() - writeAt;
      longestWriteMs = max(longestWriteMs, writeMs);
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
      if (writeMs >= 1000 || wrote == 0) {
        Serial.printf("[streamprobe] bytes=%lu requested=%u wrote=%u elapsed=%lu\n",
            (unsigned long)sentBytes, (unsigned)(result - sent), (unsigned)wrote, (unsigned long)writeMs);
        transferNetworkMemoryProbe("write-stall");
      }
#endif
      sent += wrote;
      _bytesDownloaded += wrote;
      sentBytes += wrote;
      if (!write.complete) {
        failure = transfer_sync::cancelRequested() ? "cancelled" : "socket write";
        ok = false;
        break;
      }
    }
    if (sentBytes >= nextProgress) {
      transferMemoryProbe("download-progress", sentBytes);
      Serial.printf("[xphone-os] transfer: download progress %u/%u bytes, %u ms, heap %u\n",
                    (unsigned)sentBytes, (unsigned)expectedBytes, (unsigned)(millis() - startedAt),
                    (unsigned)ESP.getFreeHeap());
      nextProgress = sentBytes + 1024 * 1024;
    }
    yield();
  }
  const bool complete = ok && sentBytes == expectedBytes;
  if (complete) client.clear();
  else abortFailedDownload(client);
  file.close();
  transferMemoryProbe("download-complete", sentBytes);
  Serial.printf("[xphone-os] transfer: download %s '%s' (%u/%u bytes)\n",
                complete ? "complete" : "interrupted", path,
                (unsigned)sentBytes, (unsigned)expectedBytes);
  Serial.printf("[xphone-os] transfer: download result=%s elapsed=%u ms maxRead=%u ms maxWrite=%u ms heap=%u\n",
                complete ? "ok" : failure, (unsigned)(millis() - startedAt),
                (unsigned)longestReadMs, (unsigned)longestWriteMs, (unsigned)ESP.getFreeHeap());
  // A guest can download too. Only the authenticated session owner may
  // cause the scene to leave STA after a broken stream.
  if (!complete && ownedBySession) _ownerTransferAborted = true;
}

// Canonical book key: drop a container extension, keep ASCII letters and
// digits, lowercase. Mirrors ReaderLibraryManager.libraryKey on both apps and
// library_key() in tools/convergence-check.py, so "The_Book_of_Elon" and
// "The Book of Elon" are ONE book to the device too. Both sides of every
// comparison run through this same function, so an accented title still
// pairs with itself even though the device cannot do the apps' NFD fold.
// One canonical-key implementation lives in FbpBook; this is a thin alias so
// the delete-siblings code below reads unchanged.
static void canonicalBookKey(const char* name, char* out, const size_t outSize) {
  reader::FbpBook::canonicalKey(name, out, outSize);
}

// Remove the four sidecars that belong to one book file, if present.
static void removeSidecars(const char* bookPath) {
  static const char* const kSide[] = {".cov", ".str", ".pos", ".bmk"};
  for (size_t i = 0; i < sizeof(kSide) / sizeof(kSide[0]); ++i) {
    char side[224];
    const int n = snprintf(side, sizeof(side), "%s%s", bookPath, kSide[i]);
    if (n > 0 && n < static_cast<int>(sizeof(side)) && SdMan.exists(side)) {
      SdMan.remove(side);
    }
  }
}

// A book is its package AND its source — but a delete must know where the
// book ENDS. The first version of this expanded a delete to every file with
// the same canonical key, and on 2026-08-22 that turned a routine variant
// cleanup into a massacre: one phone deleted the other's spelling of Elon,
// the expansion took BOTH packages and BOTH sources, and the book vanished
// from the device with all four names tombstoned. Nobody asked for that.
//
// The bounded rule:
//   1. Deleting a SOURCE expands to nothing.
//   2. Deleting a PACKAGE always takes its own same-stem source + sidecars.
//   3. It takes same-key sources under OTHER stems only when no other
//      same-key package survives — the orphan-cleanup case.
//   4. It NEVER removes another package. Under-deleting is recoverable
//      with a second tap; over-deleting is a book silently gone.
//
// Returns the number of extra files removed; each is tombstoned so no phone
// pushes half a book back.
int FileTransferServer::removeBookSiblings(const char* path) {
  if (!hasFbpExtension(path)) return 0;  // rule 1

  const char* slash = strrchr(path, '/');
  if (!slash) return 0;
  char dir[192];
  const size_t dirLen = static_cast<size_t>(slash - path);
  if (dirLen == 0 || dirLen >= sizeof(dir)) return 0;
  memcpy(dir, path, dirLen);
  dir[dirLen] = '\0';

  char wanted[96];
  canonicalBookKey(slash + 1, wanted, sizeof(wanted));
  if (wanted[0] == '\0') return 0;

  // Pass 1: does another package with this key survive? (rule 3 gate)
  bool packageSurvives = false;
  {
    FsFile d = SdMan.open(dir, O_RDONLY);
    if (!d || !d.isDir()) return 0;
    FsFile f;
    while (f.openNext(&d, O_RDONLY)) {
      char name[96];
      const int len = f.getName(name, sizeof(name));
      f.close();
      if (len <= 0 || name[0] == '.' || !hasFbpExtension(name)) continue;
      char key[96];
      canonicalBookKey(name, key, sizeof(key));
      if (strcmp(key, wanted) == 0) { packageSurvives = true; break; }
    }
    d.close();
  }

  // The deleted package's own stem, for the same-stem source (rule 2).
  char stem[96];
  {
    const size_t nameLen = strlen(slash + 1);
    const size_t stemLen = nameLen >= 4 ? nameLen - 4 : 0;  // ".fbp"
    if (stemLen == 0 || stemLen >= sizeof(stem)) return 0;
    memcpy(stem, slash + 1, stemLen);
    stem[stemLen] = '\0';
  }

  int removed = 0;
  FsFile d = SdMan.open(dir, O_RDONLY);
  if (!d || !d.isDir()) return 0;
  FsFile f;
  while (f.openNext(&d, O_RDONLY)) {
    char name[96];
    const int len = f.getName(name, sizeof(name));
    f.close();
    if (len <= 0 || name[0] == '.') continue;
    if (hasFbpExtension(name)) continue;  // rule 4: never another package
    const size_t nl = strlen(name);
    const bool isTxt = nl >= 4 && strcasecmp(name + nl - 4, ".txt") == 0;
    if (!hasEpubExtension(name) && !isTxt) continue;

    const char* dot = strrchr(name, '.');
    const size_t nameStemLen = dot ? static_cast<size_t>(dot - name) : 0;
    const bool sameStem = nameStemLen == strlen(stem) &&
                          strncmp(name, stem, nameStemLen) == 0;
    if (!sameStem) {
      if (packageSurvives) continue;  // rule 3
      char key[96];
      canonicalBookKey(name, key, sizeof(key));
      if (strcmp(key, wanted) != 0) continue;
    }

    char full[224];
    const int n = snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (n <= 0 || n >= static_cast<int>(sizeof(full))) continue;
    if (strcmp(full, path) == 0) continue;

    removeSidecars(full);
    if (SdMan.remove(full)) {
      ++removed;
      appendTombstone(full + 7);
      Serial.printf("[xphone-os] transfer: also removed %s (same book)\n", name);
    }
  }
  d.close();
  return removed;
}

void FileTransferServer::handleDelete() {
  _requestCount++;
  if (!tokenOk()) { _server->send(403, "text/plain", "Missing session token"); return; }
  char path[192];
  if (!queryPath(path, sizeof(path), /*required=*/true)) return;

  if (!SdMan.exists(path)) {
    // Echo the decoded path so the phone's error message shows exactly what
    // this server looked for — a 404 here is always a path/name mismatch.
    char msg[224];
    snprintf(msg, sizeof(msg), "Not found: %s", path);
    _server->send(404, "text/plain", msg);
    return;
  }
  // A folder can only be removed when it is EMPTY, and SdMan.remove refuses
  // directories outright, so it needs its own path. Empty-only is the whole
  // safety story here: SdMan.removeDir deletes recursively, and a delete
  // that silently took a folder full of books with it is exactly the
  // massacre removeBookSiblings was written to prevent. An upload addressed
  // to "/books/name.fbp" treats that as the destination FOLDER and creates
  // it, so stray empty folders are a thing that really happens and the
  // phone needs a way to clear them.
  {
    FsFile probe = SdMan.open(path, O_RDONLY);
    const bool isDir = probe && probe.isDir();
    bool empty = true;
    if (isDir) {
      FsFile child;
      if (child.openNext(&probe, O_RDONLY)) {
        empty = false;
        child.close();
      }
    }
    if (probe) probe.close();
    if (isDir) {
      if (!empty) {
        _server->send(409, "text/plain", "Folder is not empty");
        return;
      }
      const bool gone = SdMan.removeDir(path);
      _server->send(gone ? 200 : 500, "text/plain", gone ? "Deleted" : "Delete failed");
      return;
    }
  }

  const bool isBook = strncmp(path, "/books/", 7) == 0 &&
                      (hasEpubExtension(path) || hasFbpExtension(path));
  if (isBook) removeSidecars(path);
  if (SdMan.remove(path)) {
    if (isBook) {
      appendTombstone(path + 7);
      removeBookSiblings(path);
    }
    _server->send(200, "text/plain", "Deleted");
  } else {
    _server->send(500, "text/plain", "Delete failed");
  }
}

uint8_t* FileTransferServer::uploadWriteBuffer() {
#if defined(FLOWE_BENCH_UPLOAD_BATCH_16K)
  if (_upload.largeBatch) return _upload.largeBatch;
#endif
  return _upload.buffer;
}

size_t FileTransferServer::uploadWriteCapacity() const {
#if defined(FLOWE_BENCH_UPLOAD_BATCH_16K)
  if (_upload.largeBatch) return 16384;
#endif
  return _upload.bufferCapacity;
}

void FileTransferServer::releaseUploadBatch() {
#if defined(FLOWE_BENCH_UPLOAD_BATCH_16K)
  free(_upload.largeBatch);
  _upload.largeBatch = nullptr;
#endif
}

void FileTransferServer::closeUploadFile() {
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
  if (_uploadProfile.active) {
    UploadDuration duration(_uploadProfile.sdCloseSyncUs);
    ++_uploadProfile.closeCalls;
    if (!gUploadFile.close()) {
      ++_uploadProfile.closeFailures;
      _upload.failed = true;
    }
    return;
  }
#endif
  if (!gUploadFile.close()) _upload.failed = true;
}

#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
void FileTransferServer::reportUploadProfile(const uint64_t handlerUs) {
  const auto& p = _uploadProfile;
  // Callback time begins after the watchdog/reader check and contains
  // SD/progress time. The remaining handler time includes parsing, receive
  // waits, scheduling and response work together, not only network time.
  Serial.printf("[upload-profile] handlerCallUs=%llu callbackUs=%llu responseUs=%llu batchBytes=%u failed=%u countersBytes=%u\n",
                static_cast<unsigned long long>(handlerUs), static_cast<unsigned long long>(p.callbackUs),
                static_cast<unsigned long long>(p.responseUs), p.batchBytes, _upload.failed ? 1u : 0u,
                static_cast<unsigned>(sizeof(UploadProfile)));
  Serial.printf("[upload-profile] sdWriteUs=%llu sdWriteCalls=%u sdWrittenBytes=%llu shortWrites=%u sdCloseSyncUs=%llu closeCalls=%u closeFailures=%u explicitSyncCalls=0 progressUs=%llu progressCalls=%u responseCalls=%u\n",
                static_cast<unsigned long long>(p.sdWriteUs), p.writeCalls,
                static_cast<unsigned long long>(p.writtenBytes), p.shortWrites,
                static_cast<unsigned long long>(p.sdCloseSyncUs), p.closeCalls, p.closeFailures,
                static_cast<unsigned long long>(p.progressUs), p.progressCalls, p.responseCalls);
  Serial.printf("[upload-profile] callbacksStartWriteEndAbort=%u,%u,%u,%u payloadBytes=%llu payloadMin=%u payloadMax=%u sizesZeroLt512Lt1436Eq1436Gt1436=%u,%u,%u,%u,%u\n",
                p.callbacks[0], p.callbacks[1], p.callbacks[2], p.callbacks[3],
                static_cast<unsigned long long>(p.payloadBytes),
                p.payloadMin == UINT32_MAX ? 0u : p.payloadMin, p.payloadMax,
                p.payloadSizes[0], p.payloadSizes[1], p.payloadSizes[2], p.payloadSizes[3], p.payloadSizes[4]);
  Serial.printf("[upload-profile] callbackStartWriteEndAbortUs=%llu,%llu,%llu,%llu\n",
                static_cast<unsigned long long>(p.callbackStageUs[0]), static_cast<unsigned long long>(p.callbackStageUs[1]),
                static_cast<unsigned long long>(p.callbackStageUs[2]), static_cast<unsigned long long>(p.callbackStageUs[3]));
}
#endif

bool FileTransferServer::flushUploadBuffer() {
  if (_upload.bufferPos == 0 || !gUploadFile) return true;
  esp_task_wdt_reset();  // SD writes can be slow (FAT cluster allocation)
  size_t written;
  {
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
    UploadDuration duration(_uploadProfile.sdWriteUs);
#endif
    written = gUploadFile.write(uploadWriteBuffer(), _upload.bufferPos);
  }
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
  ++_uploadProfile.writeCalls;
  _uploadProfile.writtenBytes += written;
  if (written != _upload.bufferPos) ++_uploadProfile.shortWrites;
#endif
  const bool ok = written == _upload.bufferPos;
  _upload.bufferPos = 0;
  return ok;
}


#if defined(FLOWE_RAW_UPLOAD)
bool FileTransferServer::resumeAuthorized() {
  // Unlike legacy guest uploads, every resume operation needs both the
  // fresh BLE-issued session token and the exact reader identity.
  const String target = _server->header("X-Flowe-Reader-Id");
  if (!gSessionToken[0] || !tokenOk() || target.length() != 12 || !targetReaderOk()) {
    _server->send(401, "application/json", "{\"error\":\"Resume authorization required\"}");
    return false;
  }
#if defined(FLOWE_SYNC_FAST_SDK)
  if (!transfer_sync::memoryReleased()) {
    _server->send(503, "application/json", "{\"error\":\"Get reader status first\"}");
    return false;
  }
#endif
  _verifiedContact = true;
  // handleClient can replace the transfer buffer after the readiness
  // barrier. Never retain its old address across separate requests.
  gResumeStorage.configure(_upload.buffer, _upload.bufferCapacity);
  return true;
}

void FileTransferServer::sendResumeResult(int value) {
  using flowe_resume::Result;
  const Result result = Result(value);
  _requestCount++;
  _server->sendHeader("Cache-Control", "no-store");
  if (result != Result::ok) {
    int code = 500;
    const char* error = "Resume storage error";
    switch (result) {
      case Result::notFound: code = 404; error = "Resume slot not found"; break;
      case Result::invalid: code = 400; error = "Invalid resume request"; break;
      case Result::unauthorized: code = 401; error = "Resume identity mismatch"; break;
      case Result::conflict: code = 409; error = "Resume slot, content, or offset conflict"; break;
      case Result::hashMismatch: code = 422; error = "Resume content checksum mismatch"; break;
      default: break;
    }
    char response[112];
    snprintf(response, sizeof(response), "{\"error\":\"%s\"}", error);
    _server->send(code, "application/json", response);
    return;
  }
  const auto& record = gResumeUpload.record();
  using flowe_resume::State;
  const char* status = gResumeUpload.state() == State::complete ? "complete" :
      gResumeUpload.state() == State::cancelled ? "cancelled" :
      gResumeUpload.state() == State::failed ? "failed" : "receiving";
  char response[256];
  snprintf(response, sizeof(response),
      "{\"id\":\"%s\",\"status\":\"%s\",\"offset\":%lu,\"size\":%lu,\"sha256\":\"%s\"}",
      record.id, status, static_cast<unsigned long>(record.offset),
      static_cast<unsigned long>(record.size), record.sha256);
  _server->send(200, "application/json", response);
}

int FileTransferServer::startResumeFromRequest(bool initialBody) {
  using flowe_resume::Result;
  const String reader = _server->header("X-Flowe-Reader-Id");
  const String id = _server->header("X-Flowe-Resume-Id");
  const String secret = _server->header("X-Flowe-Resume-Secret");
  const String directory = _server->arg("path");
  const String name = _server->arg("name");
  const String sizeText = _server->header("X-Flowe-Resume-Size");
  const String digest = _server->header("X-Flowe-Resume-SHA256");
  uint32_t size = 0;
  if (directory.length() == 0 || directory.length() > 126 || name.length() == 0 || name.length() > 96 ||
      strlen(directory.c_str()) != directory.length() || strlen(name.c_str()) != name.length() ||
      name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
      sizeText.length() > 10 || strlen(sizeText.c_str()) != sizeText.length() || digest.length() != 64 ||
      !flowe_resume::decimal(sizeText.c_str(), size)) {
    return int(Result::invalid);
  }
  char target[192];
  const int length = snprintf(target, sizeof(target), "%s/%s", directory.c_str(), name.c_str());
  if (length < 0 || size_t(length) >= sizeof(target)) { return int(Result::invalid); }
  if (initialBody && size != _server->clientContentLength()) return int(Result::invalid);
  return int(gResumeUpload.start(reader.c_str(), target, size, digest.c_str(), id.c_str(), secret.c_str(), initialBody));
}

void FileTransferServer::handleResumeControl(unsigned action) {
  using flowe_resume::Result;
  if (!resumeAuthorized()) return;
  if (action == 3) {
    const String intent = _server->header("X-Flowe-Resume-Reset");
    if (intent.length() != 7 || intent != "discard") {
      sendResumeResult(int(Result::invalid)); return;
    }
    const Result result = gResumeUpload.reset();
    if (result != Result::ok) { sendResumeResult(int(result)); return; }
    _requestCount++;
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", "{\"reset\":true}");
    return;
  }
  const String reader = _server->header("X-Flowe-Reader-Id");
  const String id = _server->header("X-Flowe-Resume-Id");
  const String secret = _server->header("X-Flowe-Resume-Secret");
  if (id.length() != 32 || secret.length() != 64 ||
      !flowe_resume::hex(id.c_str(), 32) || !flowe_resume::hex(secret.c_str(), 64)) {
    sendResumeResult(int(Result::invalid)); return;
  }
  Result result;
  if (action == 0) {
    result = Result(startResumeFromRequest(false));
  } else if (action == 1) {
    result = gResumeUpload.status(reader.c_str(), id.c_str(), secret.c_str());
  } else {
    result = gResumeUpload.cancel(reader.c_str(), id.c_str(), secret.c_str());
  }
  sendResumeResult(int(result));
}

void FileTransferServer::handleResumeRaw(HTTPRaw& raw) {
  using flowe_resume::Result;
  auto reject = [this](Result result) {
    gResumeUpload.abort();
    _resumeRawComplete = false;
    sendResumeResult(int(result));
    _server->client().stop();
  };
  if (raw.status == RAW_START) {
    _resumeRawComplete = false;
    _resumeBodyReceived = 0;
    if (!resumeAuthorized()) { _server->client().stop(); return; }
    const String reader = _server->header("X-Flowe-Reader-Id");
    const String id = _server->header("X-Flowe-Resume-Id");
    const String secret = _server->header("X-Flowe-Resume-Secret");
    const String offsetText = _server->header("X-Flowe-Resume-Offset");
    uint32_t offset = 0;
    const size_t length = _server->clientContentLength();
    if (id.length() != 32 || secret.length() != 64 || !flowe_resume::hex(id.c_str(), 32) ||
        !flowe_resume::hex(secret.c_str(), 64) || offsetText.length() > 10 ||
        strlen(offsetText.c_str()) != offsetText.length() || !flowe_resume::decimal(offsetText.c_str(), offset) ||
        _server->header("Content-Type") != "application/octet-stream" || !length || length > flowe_resume::kMaxBytes) {
      reject(Result::invalid); return;
    }
    _rawExpectedBytes = uint32_t(length);
    if (_server->hasHeader("X-Flowe-Resume-Size") || _server->hasHeader("X-Flowe-Resume-SHA256")) {
      // A foreground-created OS background cohort can queue complete book
      // requests now. Each request creates its own slot only when the
      // single-client server reaches it after the previous book completes.
      if (offset != 0) { reject(Result::conflict); return; }
      const Result started = Result(startResumeFromRequest(true));
      if (started != Result::ok) { reject(started); return; }
    }
    const Result result = gResumeUpload.begin(reader.c_str(), id.c_str(), secret.c_str(), offset, _rawExpectedBytes);
    if (result != Result::ok) { reject(result); return; }
    _hookMark = _bytesUploaded;
    if (progressHook) progressHook();
    return;
  }
  if (!gResumeUpload.active()) return;
  if (raw.status == RAW_WRITE) {
    if (!raw.currentSize || raw.totalSize != _resumeBodyReceived + raw.currentSize || raw.totalSize > _rawExpectedBytes) {
      reject(Result::invalid); return;
    }
    const Result result = gResumeUpload.append(raw.buf, raw.currentSize);
    if (result != Result::ok) { reject(result); return; }
    _resumeBodyReceived += raw.currentSize;
    _bytesUploaded += raw.currentSize;
    if (progressHook && _bytesUploaded - _hookMark >= 4u * 1024u * 1024u) {
      _hookMark = _bytesUploaded;
      progressHook();
    }
  } else if (raw.status == RAW_END) {
    if (raw.totalSize != _rawExpectedBytes || _resumeBodyReceived != _rawExpectedBytes) { reject(Result::invalid); return; }
    _resumeResult = int(gResumeUpload.finish());
    _resumeRawComplete = true;
    Serial.printf("[resume-upload] body finished bytes=%lu result=%d\n",
                  static_cast<unsigned long>(_resumeBodyReceived), _resumeResult);
  } else if (raw.status == RAW_ABORTED) {
    gResumeUpload.abort();
    _resumeRawComplete = false;
    // Retain this Wi-Fi session for a retry. Legacy uploads still use the
    // old abort flag. This path owns a durable checkpoint and its secret.
    Serial.println("[resume-upload] interrupted; checkpoint retained");
  }
}

void FileTransferServer::rejectRawUpload(const char* reason, int code) {
  // Only map ABORT when this request entered START. No stale path is removed
  // for a rejected header/query. The common abort drops the owned .part.
  if (_rawUploadActive) {
    HTTPUpload& upload = _server->upload();
    upload.status = UPLOAD_FILE_ABORTED;
    upload.currentSize = 0;
    handleUploadData();
  }
  _rawUploadActive = false;
  _rawUploadComplete = false;
  _upload.failed = true;
  Serial.printf("[raw-upload] rejected: %s\n", reason);
  _server->send(code, "text/plain", reason);
  // Do not let the parser read the rejected body. Its next raw read emits
  // ABORT; the inactive guard below prevents a second file operation.
  _server->client().stop();
}

void FileTransferServer::handleRawUpload(HTTPRaw& raw) {
  if (raw.status == RAW_START) {
    _resumeRawRequest = _server->hasHeader("X-Flowe-Resume-Id") || _server->hasHeader("X-Flowe-Resume-Secret") ||
        _server->hasHeader("X-Flowe-Resume-Offset") || _server->hasHeader("X-Flowe-Resume-Size") ||
        _server->hasHeader("X-Flowe-Resume-SHA256");
    _resumeRawComplete = false;
    _rawUploadActive = _rawUploadComplete = false;
  }
  if (_resumeRawRequest) { handleResumeRaw(raw); return; }
  static_assert(HTTP_RAW_BUFLEN <= HTTP_UPLOAD_BUFLEN,
                "Raw chunks must fit the reserved multipart callback buffer");
  HTTPUpload& upload = _server->upload();
#if defined(FLOWE_SYNC_FAST_SDK)
  if (raw.status == RAW_START && !transfer_sync::memoryReleased()) {
    rejectRawUpload("Get reader status before file transfer", 503);
    return;
  }
#endif
  if (raw.status == RAW_START) {
    _rawUploadActive = false;
    _rawUploadComplete = false;
    _rawExpectedBytes = 0;
    if (!targetReaderOk()) {
      rejectRawUpload("Reader identity mismatched", 409);
      return;
    }
    if (_server->header("Content-Type") != "application/octet-stream" ||
        _server->args() != 2 || !_server->hasArg("path") || !_server->hasArg("name")) {
      rejectRawUpload("Expected octet-stream and path/name arguments", 400);
      return;
    }
    const String name = _server->arg("name");
    if (_server->clientContentLength() <= 0) {
      rejectRawUpload("Invalid raw length", 400);
      return;
    }
    _rawExpectedBytes = static_cast<uint32_t>(_server->clientContentLength());
    upload.status = UPLOAD_FILE_START;
    upload.name = "file";
    upload.filename = name;
    upload.type = "application/octet-stream";
    upload.totalSize = 0;
    upload.currentSize = 0;
    if (upload.filename != name) {
      rejectRawUpload("Reader busy; filename allocation failed", 503);
      return;
    }
    _rawUploadActive = true;
    handleUploadData();
    if (_upload.failed || !_upload.fileOpen) rejectRawUpload("Raw upload start failed", 400);
    else if (_rawExpectedBytes >= 5744 &&
             BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4) {
      // Keep prefetched body bytes. Allocation failure retains the old buffer.
      _server->client().floweGrowReadBuffer(5744);
    }
    return;
  }
  if (!_rawUploadActive) return;
  if (raw.status == RAW_WRITE) {
    if (_upload.failed || raw.currentSize == 0 || raw.currentSize > sizeof(upload.buf) ||
        raw.totalSize > _rawExpectedBytes ||
        raw.totalSize != _upload.received + raw.currentSize) {
      rejectRawUpload("Raw byte count mismatch", 400);
      return;
    }
    memcpy(upload.buf, raw.buf, raw.currentSize);
    upload.status = UPLOAD_FILE_WRITE;
    upload.currentSize = raw.currentSize;
    upload.totalSize = raw.totalSize;
    handleUploadData();
    if (_upload.failed) rejectRawUpload("Raw SD write failed", 500);
  } else if (raw.status == RAW_END) {
    if (_upload.failed || raw.totalSize != _rawExpectedBytes || _upload.received != _rawExpectedBytes) {
      rejectRawUpload("Incomplete raw upload", 400);
      return;
    }
    upload.status = UPLOAD_FILE_END;
    upload.currentSize = 0;
    upload.totalSize = raw.totalSize;
    // The common END checks flush/close before journaled publication.
    // Multipart and raw bodies use the same failure and recovery rules.
    handleUploadData();
    if (_upload.failed) SdMan.remove(_upload.part);
    _rawUploadActive = false;
    _rawUploadComplete = true;
  } else if (raw.status == RAW_ABORTED) {
    upload.status = UPLOAD_FILE_ABORTED;
    upload.currentSize = 0;
    handleUploadData();
    _rawUploadActive = false;
    _rawUploadComplete = false;
  }
}
#endif

void FileTransferServer::handleUploadData() {
  esp_task_wdt_reset();
  // Multipart callbacks run before middleware. Reject every stage before
  // opening a file, promoting a .part, or changing the owner-abort event.
  if (!targetReaderOk()) return;
  const HTTPUpload& up = _server->upload();
#if defined(FLOWE_SYNC_FAST_SDK)
  if (up.status == UPLOAD_FILE_START && !transfer_sync::memoryReleased()) {
    _upload.failed = true;
    _server->client().stop();
    return;
  }
#endif
  if (transfer_sync::pollControls() && up.status != UPLOAD_FILE_ABORTED) {
    // No publication on physical/USB cancellation, including a final chunk.
    _upload.failed = true;
    _upload.bufferPos = 0;
    if (_upload.fileOpen) { closeUploadFile(); _upload.fileOpen = false; SdMan.remove(_upload.part); }
    releaseUploadBatch();
    _server->client().stop();
    return;
  }
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
  if (!_uploadProfile.active) {
    _uploadProfile = {};
    _uploadProfile.active = true;
  }
  const unsigned status = static_cast<unsigned>(up.status);
  UploadDuration callbackDuration(_uploadProfile.callbackUs,
                                  status < 4 ? &_uploadProfile.callbackStageUs[status] : nullptr);
  if (status < 4) ++_uploadProfile.callbacks[status];
  if (up.status == UPLOAD_FILE_WRITE) {
    const uint32_t size = up.currentSize;
    _uploadProfile.payloadBytes += size;
    if (size < _uploadProfile.payloadMin) _uploadProfile.payloadMin = size;
    if (size > _uploadProfile.payloadMax) _uploadProfile.payloadMax = size;
    const unsigned bucket = size == 0 ? 0 : size < 512 ? 1 : size < 1436 ? 2 : size == 1436 ? 3 : 4;
    ++_uploadProfile.payloadSizes[bucket];
  }
#endif

  if (up.status == UPLOAD_FILE_START) {
    allocationProbePhase(6);
    transferMemoryProbe("upload-start");
    // Uploads are open to guests; ending the session is not. Capture the
    // same authority as /stop before the request's headers go away.
    _upload.ownedBySession = tokenOk();
    releaseUploadBatch();
    _upload.bufferPos = 0;
    _upload.failed = false;
    _upload.fileOpen = false;
    _upload.received = 0;

    char dir[128];
    if (_server->arg("path").length() >= sizeof(dir) - 1 ||
        strlen(_server->arg("path").c_str()) != _server->arg("path").length()) {
      _upload.failed = true;
      return;
    }
    if (!_server->hasArg("path")) {
      snprintf(dir, sizeof(dir), "/books");
    } else {
      const String& arg = _server->arg("path");
      snprintf(dir, sizeof(dir), "%s%s", arg.startsWith("/") ? "" : "/", arg.c_str());
      size_t len = strlen(dir);
      while (len > 1 && dir[len - 1] == '/') dir[--len] = '\0';
    }
    if (!isSafePath(dir) || up.filename.length() == 0 || isHiddenName(up.filename.c_str()) ||
        up.filename.indexOf('/') >= 0 || up.filename.indexOf('\\') >= 0 ||
        up.filename.indexOf("..") >= 0 || strlen(up.filename.c_str()) != up.filename.length()) {
      _upload.failed = true;
      return;
    }
    // Names longer than the shelf's path budget store fine on FAT but are
    // invisible to every listing afterwards — the upload "succeeded" and the
    // book never appeared (audit I1, 2026-08-18). Refuse them honestly.
    if (up.filename.length() > 96) {
      Serial.printf("[xphone-os] transfer: name too long (%u chars), refused\n",
                    static_cast<unsigned>(up.filename.length()));
      _upload.failed = true;
      return;
    }
    if (!SdMan.exists(dir) && !SdMan.mkdir(dir)) {
      Serial.printf("[xphone-os] transfer: mkdir %s failed\n", dir);
      _upload.failed = true;
      return;
    }
    if (strlen(dir) + 1 + up.filename.length() >= sizeof(_upload.path)) {
      _upload.failed = true;
      return;
    }
    snprintf(_upload.path, sizeof(_upload.path), "%s/%s", dir, up.filename.c_str());
    // Write to a .part file and rename only on a clean end. A reset or a
    // dropped connection mid-upload used to leave a truncated file under the
    // real name; the device listed it and both phones read it as the book
    // (0-byte "PHM Cover Test.fbp" and a bad-magic Ikigai, 2026-09-06).
    snprintf(_upload.part, sizeof(_upload.part), "%s.part", _upload.path);

    esp_task_wdt_reset();
    if (SdMan.exists(_upload.part)) SdMan.remove(_upload.part);
    gUploadFile = SdMan.open(_upload.part, O_WRONLY | O_CREAT | O_TRUNC);
    if (!gUploadFile) {
      Serial.printf("[xphone-os] transfer: create %s failed\n", _upload.path);
      _upload.failed = true;
      return;
    }
    _upload.fileOpen = true;
#if defined(FLOWE_BENCH_UPLOAD_BATCH_16K)
    // Allocate only for this file, after .part opened successfully. Never
    // resize/reassign the 4 KiB RTC scratch shared by the other handlers.
    _upload.largeBatch = static_cast<uint8_t*>(malloc(16384));
    Serial.printf("[upload-batch] requested=16384 selected=%u allocation=%s extraHeap=%u freeHeap=%u\n",
                  static_cast<unsigned>(uploadWriteCapacity()), _upload.largeBatch ? "ok" : "fallback",
                  _upload.largeBatch ? 16384u : 0u, ESP.getFreeHeap());
#endif
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
    _uploadProfile.batchBytes = uploadWriteCapacity();
#endif
    Serial.printf("[xphone-os] transfer: upload start %s\n", _upload.path);
    _hookMark = _bytesUploaded;
    if (progressHook) {
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
      UploadDuration duration(_uploadProfile.progressUs);
      ++_uploadProfile.progressCalls;
#endif
      progressHook();
#if defined(FLOWE_BENCH_TRANSFER_FLUSH_BARRIER)
      waitTransferDisplay("upload-start-display");
#endif
    }
    transferMemoryProbe("upload-after-open-and-display");

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (_upload.failed || !_upload.fileOpen) return;
    const uint8_t* data = up.buf;
    size_t remaining = up.currentSize;
    uint8_t* const batch = uploadWriteBuffer();
    const size_t batchSize = uploadWriteCapacity();
    while (remaining > 0) {
      const size_t space = batchSize - _upload.bufferPos;
      const size_t toCopy = remaining < space ? remaining : space;
      memcpy(batch + _upload.bufferPos, data, toCopy);
      _upload.bufferPos += toCopy;
      data += toCopy;
      remaining -= toCopy;
      if (_upload.bufferPos >= batchSize && !flushUploadBuffer()) {
        _upload.failed = true;
        closeUploadFile();
        releaseUploadBatch();
        _upload.fileOpen = false;
        SdMan.remove(_upload.part);
        return;
      }
    }
    const uint32_t priorReceived = _upload.received;
    _upload.received += up.currentSize;
    if ((priorReceived >> 18) != (_upload.received >> 18))
      transferMemoryProbe("upload-progress", _upload.received);
    _bytesUploaded += up.currentSize;
    feedLoopWDT();  // a whole book arrives inside one handleClient(); the loop watchdog must not count it as a hang
    // ~4 MB cadence: at bench speed (~165 KB/s) that is one e-ink repaint
    // every ~25 s — visible progress for ~3% throughput cost.
    if (progressHook && _bytesUploaded - _hookMark >= 4u * 1024u * 1024u) {
      _hookMark = _bytesUploaded;
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
      UploadDuration duration(_uploadProfile.progressUs);
      ++_uploadProfile.progressCalls;
#endif
      progressHook();
#if defined(FLOWE_BENCH_TRANSFER_FLUSH_BARRIER)
      waitTransferDisplay("upload-progress-display");
#endif
    }

  } else if (up.status == UPLOAD_FILE_END) {
    transferMemoryProbe("upload-before-close", _upload.received);
    if (_upload.fileOpen) {
      const bool completed = flowe_upload::finish(_upload.failed,
          [this] { return flushUploadBuffer(); },
          [this] { closeUploadFile(); _upload.fileOpen = false; return !_upload.failed; },
          [this] { return publishUpload(_upload.part, _upload.path); });
      _upload.failed = !completed;
    } else {
      _upload.failed = true;
    }
    releaseUploadBatch();
    if (_upload.failed) SdMan.remove(_upload.part);
    if (!_upload.failed) {
      Serial.printf("[xphone-os] transfer: upload done %s (%u bytes)\n", _upload.path,
                    static_cast<unsigned>(_upload.received));
      // A replaced package must not keep the OLD book's shelf art: the
      // sidecar extractor early-returns when either file exists, so a
      // recompiled book would wear its predecessor's cover and title strip
      // forever. Dropping them here makes the next shelf visit re-extract
      // from the new package.
      const size_t plen = strlen(_upload.path);
      if (plen >= 4 && strcasecmp(_upload.path + plen - 4, ".fbp") == 0) {
        char side[sizeof(_upload.path) + 4];
        snprintf(side, sizeof(side), "%s.cov", _upload.path);
        if (SdMan.exists(side)) SdMan.remove(side);
        snprintf(side, sizeof(side), "%s.str", _upload.path);
        if (SdMan.exists(side)) SdMan.remove(side);
      }
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    _upload.bufferPos = 0;
    if (_upload.fileOpen) {
      closeUploadFile();
      _upload.fileOpen = false;
      SdMan.remove(_upload.part);  // drop the partial file
    }
    releaseUploadBatch();
    _upload.failed = true;
    Serial.println("[xphone-os] transfer: upload aborted");
    if (_upload.ownedBySession) _ownerTransferAborted = true;
  }
}

// The reader's place in a raw epub survives the upgrade to a package. The
// epub's cache holds {spine, page, section pages} (progress.bin) and the
// spine count (book.bin header); both are tiny reads, safe in the
// transfer-mode heap. The fraction lands as a .pos in a neutral
// 10,000-page pagination — every consumer (firmware loadPos, phone
// harvest) rescales via the trailer. Chapter lengths vary, so this is a
// same-chapter landing, not a same-word one; it always rounds down so
// furthest-wins can never jump a reader forward. If book.bin's version
// moves past v8, the handoff quietly stops until this reader learns the
// new header.
static void handoffEpubPosition(const char* epubPath, const char* fbpPath) {
  char posPath[196];
  snprintf(posPath, sizeof(posPath), "%s.pos", fbpPath);
  if (SdMan.exists(posPath)) return;  // a real position always wins

  const std::string cache = std::string(reader::kReaderCacheRoot) + "/epub_" +
                            std::to_string(std::hash<std::string>{}(std::string(epubPath)));
  FsFile pf = SdMan.open((cache + "/progress.bin").c_str(), O_RDONLY);
  if (!pf) return;
  uint8_t d[6] = {0};
  const int n = pf.read(d, sizeof(d));
  pf.close();
  if (n != 4 && n != 6) return;
  const uint16_t spine = (uint16_t)(d[0] | (d[1] << 8));
  const uint16_t page = (uint16_t)(d[2] | (d[3] << 8));
  const uint16_t secPages = (n == 6) ? (uint16_t)(d[4] | (d[5] << 8)) : 0;

  FsFile bf = SdMan.open((cache + "/book.bin").c_str(), O_RDONLY);
  if (!bf) return;
  uint8_t hdr[7] = {0};
  const int hn = bf.read(hdr, sizeof(hdr));
  bf.close();
  if (hn != 7 || hdr[0] != 8) return;  // book.bin v8 header only
  const uint16_t spineCount = (uint16_t)(hdr[5] | (hdr[6] << 8));
  if (spineCount == 0 || spine >= spineCount) return;

  float frac = (float)spine / (float)spineCount;
  if (secPages > 0 && page < secPages)
    frac += ((float)page / (float)secPages) / (float)spineCount;
  if (frac <= 0.0f || frac >= 1.0f) return;

  uint32_t p32 = (uint32_t)(frac * 10000.0f);
  const uint32_t c32 = 10000;
  if (p32 == 0) return;
  FsFile out = SdMan.open(posPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) return;
  out.write(&p32, 4);
  out.write(&c32, 4);
  out.close();
  Serial.printf("[xphone-os] transfer: epub position handed off (%lu/10000) -> %s\n",
                (unsigned long)p32, posPath);
}

#if defined(FLOWE_RAW_UPLOAD)
static bool finishResumeBook(const char* path) {
  if (!hasFbpExtension(path)) return true;
  char side[196];
  for (const char* extension : {".cov", ".str"}) {
    snprintf(side, sizeof(side), "%s%s", path, extension);
    if (SdMan.exists(side) && !SdMan.remove(side)) return false;
  }
  char sibling[192];
  snprintf(sibling, sizeof(sibling), "%s", path);
  char* dot = strrchr(sibling, '.');
  snprintf(dot, sizeof(sibling) - size_t(dot - sibling), ".epub");
  if (SdMan.exists(sibling)) handoffEpubPosition(sibling, path);
  return true;
}
#endif

void FileTransferServer::handleUploadDone() {
#if defined(FLOWE_BENCH_UPLOAD_PROFILE)
  UploadDuration duration(_uploadProfile.responseUs);
  ++_uploadProfile.responseCalls;
#endif
  transferMemoryProbe("upload-response", _upload.received);
  Serial.printf("[xphone-os] transfer: upload done heap=%u largest=%u\n", ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  _requestCount++;
  if (_upload.failed) {
    _server->send(400, "text/plain", "Upload failed");
    return;
  }
  // An optimized package takes over from its raw sideloaded epub: same
  // directory, same stem. The reading position moves across, and the
  // shelf shows one book, not two.
  //
  // 2026-08-20: the epub is now KEPT, where it used to be deleted and
  // tombstoned. Andrew: "we should make sure we never remove the epub
  // files then from the SD cards right?" He is right, and the cost is
  // small — a source is about 3% of its package (Karamazov 1.2 MB
  // against 32.6 MB, and against 8.9 MB now that packages are v4).
  //
  // What keeping it buys: ANY phone can rebuild ANY book, instead of
  // only the phone that happens to hold the original. That is the real
  // root of the two-phone problem, and it is also how a colour cover
  // reaches a phone that never imported the book. The card becomes the
  // master library rather than a cache of one phone's.
  //
  // The shelf hides an epub whose .fbp sits beside it (ReaderScene
  // scanDir), so there is still no double Sherlock.
  if (hasFbpExtension(_upload.path)) {
    char sibling[sizeof(_upload.path)];
    snprintf(sibling, sizeof(sibling), "%s", _upload.path);
    char* dot = strrchr(sibling, '.');
    const size_t room = sizeof(sibling) - (dot - sibling);
    snprintf(dot, room, ".epub");
    if (SdMan.exists(sibling)) {
      handoffEpubPosition(sibling, _upload.path);
      Serial.printf("[xphone-os] transfer: package took over from %s (source kept)\n", sibling);
    }
  }
  _server->send(200, "text/plain", "File uploaded");
}

// 0.6 workstream C — the sync manifest. One GET answers "what books does
// the device hold, exactly": path relative to /books (one subdir level,
// matching the shelf scanner), byte size, and the MD5 of the first 64 KB
// (cheap identity; whole-file hashing would take minutes on big cards).
// Tombstones ride along so the app learns about deletes in the same call.
// A book named  The "Good" Parts.epub  broke the whole manifest into
// unparseable JSON, so sync silently moved nothing. (Release audit,
// 2026-08-18.) The shelf path already escapes via ArduinoJson; this is the
// same job for the streaming writer.
static void appendEscaped(String& out, const char* s) {
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') out += '\\';
    out += *s;
  }
}

void FileTransferServer::handleManifest() {
  _requestCount++;
  _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server->send(200, "application/json", "");
  // The synchronous server cannot upload while it builds a manifest. Reuse
  // its reserved buffer rather than add another heap or task-stack buffer.
  static_assert(UploadState::kBufferSize >= HttpResponseBatch::kStorageSize);
  NetworkClient client = _server->client();
  HttpResponseBatch batch(_upload.buffer, _server->version() != "HTTP/1.0",
      [](void* context, const uint8_t* bytes, size_t length) {
        return static_cast<NetworkClient*>(context)->write(bytes, length);
      }, &client);
  bool alive = batch.append("{\"books\":[");

  bool first = true;
  char rel[192];
  const auto emitDir = [&](const char* dir, const char* prefix) -> bool {
    FsFile d = SdMan.open(dir, O_RDONLY);
    if (!d || !d.isDir()) return true;
    FsFile f;
    while (f.openNext(&d, O_RDONLY)) {
      esp_task_wdt_reset();
      char name[128];
      const int len = f.getName(name, sizeof(name));
      if (len <= 0 || len >= static_cast<int>(sizeof(name)) - 1 || name[0] == '.' || f.isDir()) {
        f.close();
        continue;
      }
      if (!hasEpubExtension(name) && !hasFbpExtension(name)) {
        f.close();
        continue;
      }
      const uint32_t size = f.fileSize();
      // First-64KB MD5 through the already-open handle.
      MD5Builder md5;
      md5.begin();
      uint8_t buf[1024];
      uint32_t left = 65536;
      while (left > 0) {
        const int n = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n <= 0) break;
        md5.add(buf, static_cast<uint16_t>(n));
        left -= static_cast<uint32_t>(n);
      }
      md5.calculate();
      f.close();
      snprintf(rel, sizeof(rel), "%s%s", prefix, name);
      String row = first ? "{\"name\":\"" : ",{\"name\":\"";
      appendEscaped(row, rel);
      char tail[96];
      snprintf(tail, sizeof(tail), "\",\"size\":%lu,\"md5h\":\"%s\"}",
               static_cast<unsigned long>(size), md5.toString().c_str());
      row += tail;
      if (!batch.append(row.c_str(), row.length())) { d.close(); return false; }
      first = false;
    }
    d.close();
    return true;
  };

  if (alive) alive = emitDir("/books", "");
  // One subdir level, same rule as the shelf.
  if (alive) {
    FsFile d = SdMan.open("/books", O_RDONLY);
    if (d && d.isDir()) {
      FsFile f;
      while (alive && f.openNext(&d, O_RDONLY)) {
        char name[128];
        const int len = f.getName(name, sizeof(name));
        const bool usable = len > 0 && len < static_cast<int>(sizeof(name)) - 1 && name[0] != '.' && f.isDir();
        f.close();
        if (!usable) continue;
        char sub[160], prefix[144];
        if (snprintf(sub, sizeof(sub), "/books/%s", name) >= static_cast<int>(sizeof(sub))) continue;
        snprintf(prefix, sizeof(prefix), "%s/", name);
        alive = emitDir(sub, prefix);
      }
      d.close();
    }
  }

  if (alive) alive = batch.append("],\"tombstones\":[");
  first = true;
  if (alive) {
    FsFile t = SdMan.open(kTombstonePath, O_RDONLY);
    if (t) {
      char line[192];
      size_t pos = 0;
      int c;
      while (alive && (c = t.read()) >= 0) {
        if (c == '\n' || pos >= sizeof(line) - 1) {
          line[pos] = 0;
          if (pos > 0) {
            String row = first ? "\"" : ",\"";
            appendEscaped(row, line);
            row += '"';
            alive = batch.append(row.c_str(), row.length());
            first = false;
          }
          pos = 0;
        } else {
          line[pos++] = static_cast<char>(c);
        }
      }
      t.close();
    }
  }
  if (alive) alive = batch.append("]}") && batch.flush();
  if (alive) {
    _server->sendContent("");  // Framework owns the final chunk and its state.
  } else {
    Serial.println("[xphone-os] transfer: manifest: client stopped taking bytes; dropping it");
    client.stop();
  }
}
