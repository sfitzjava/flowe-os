"""Compile the production status authority decision with sanitizer checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class StatusAuthorizationTest(unittest.TestCase):
    def test_status_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "status-auth"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "src/net"),
                            str(ROOT / "test/host/status_authorization_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    def test_fallback_ignores_public_or_rejected_requests(self):
        source = (ROOT / "src/scenes/FileTransferScene.cpp").read_text()
        begin = source.index("      if (!_directMode && (_targetGiven || _hotspotFallback)")
        end = source.index(" {", begin)
        condition = source[begin:end].strip()[4:-1]
        harness = r'''
#include <cassert>
unsigned millis() { return 30001; }
struct Server { bool contact; unsigned requests; bool verifiedContact() { return contact; }
unsigned requestCount() { return requests; } } _server;
bool _directMode = false, _targetGiven = false, _hotspotFallback = true;
unsigned _servedSinceMs = 1000, kKnockTimeoutMs = 20000;
bool fallback() { return ''' + condition + r'''; }
int main() {
  _server = {false, 0}; assert(fallback());
  _server = {false, 7}; assert(fallback());
  _server = {true, 7}; assert(!fallback());
  _server = {false, 7}; _directMode = true; assert(!fallback());
  _directMode = false; _hotspotFallback = false; assert(!fallback());
  _targetGiven = true; assert(fallback());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "fallback.cpp"
            binary = Path(directory) / "fallback"
            code.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(code), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_actual_handler_authorizes_before_hooks_and_metadata(self):
        source = (ROOT / "src/net/FileTransferServer.cpp").read_text()
        begin = source.index("void FileTransferServer::handleStatus() {")
        end = source.index("  JsonDocument doc;", begin)
        prefix = source[begin:end]
        self.assertIn('doc["sessionVerified"] = authorization.sessionVerified;', source[end:end + 160])
        setter = source[source.index("void transferSetSessionToken("):source.index("\nnamespace {", source.index("void transferSetSessionToken("))]
        harness = r'''
#include "StatusAuthorization.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#define FLOWE_SYNC_FAST_SDK
class String {
 public:
  std::string value;
  String(std::string input): value(input) {}
  const char* c_str() const { return value.c_str(); }
  size_t length() const { return value.size(); }
};
char gSessionToken[48];
bool gOwnedReadiness = true;
struct SerialStub { void printf(const char*, unsigned) {} } Serial;
''' + setter + r'''
struct Server {
  std::string token, reader;
  int response = 0;
  String header(const char* key) { return !strcmp(key,"X-Flowe-Token") ? token : reader; }
  void send(int status, const char*, const char*) { response = status; }
};
namespace WifiCreds {
constexpr size_t kReaderIdSize = 13;
bool fail = false;
bool readerId(char* target, size_t) { if (fail) return false; strcpy(target,"F85B1BFC2E18"); return true; }
}
namespace transfer_sync { bool released = true; bool memoryReleased() { return released; } }
unsigned hooks = 0;
bool ready = true;
bool prepare(void*) { ++hooks; return ready; }
struct FileTransferServer {
  Server* _server;
  unsigned _requestCount = 0;
  bool _verifiedContact = false, metadataReached = false, observedVerified = false;
  bool (*prepareStatusHook)(void*) = prepare;
  void* prepareStatusContext = nullptr;
  void handleStatus();
};
''' + prefix + r'''
  metadataReached = true;
  observedVerified = authorization.sessionVerified;
}
void run(const char* sentToken, const char* sentReader, const char* owner,
         int expectedCode, unsigned expectedHooks, bool metadata, bool verified) {
  strcpy(gSessionToken, owner); hooks = 0;
  Server server{sentToken, sentReader}; FileTransferServer handler{&server};
  handler.handleStatus();
  assert(server.response == expectedCode && hooks == expectedHooks && handler.metadataReached == metadata);
  assert(handler.observedVerified == verified && handler._requestCount == 1);
  assert(handler._verifiedContact == (expectedHooks != 0));
}
int main() {
  transferSetSessionToken("legacy", false);
  assert(!gOwnedReadiness && !strcmp(gSessionToken, "legacy"));
  transferSetSessionToken("modern", true);
  assert(gOwnedReadiness && !strcmp(gSessionToken, "modern"));
  transferSetSessionToken("legacy", false);
  transferSetSessionToken("", false);
  assert(gOwnedReadiness && !gSessionToken[0]);
  transferSetSessionToken("legacy", false);
  transferSetSessionToken(nullptr, false);
  assert(gOwnedReadiness && !gSessionToken[0]);
  const char* local = "F85B1BFC2E18";
  run("wrong",local,"owner",401,0,false,false);
  run("owner",local,"",401,0,false,false);
  run("owner","F85B1BFC2E19","owner",401,0,false,false);
  run("owner","","owner",401,0,false,false);
  run("","F85B1BFC2E19","",401,0,false,false);
  run("","","owner",0,0,true,false);
  run("","","",0,1,true,false);
  run("owner",local,"owner",0,1,true,true);
  gOwnedReadiness = false;
  run("owner","","owner",0,1,true,false);
  run("","","owner",0,1,true,false);
  run("wrong","","owner",401,0,false,false);
  run("owner",local,"owner",0,1,true,true);
  ready = false; run("","","owner",503,1,false,false); ready = true;
  gOwnedReadiness = true;
  ready = false; run("owner",local,"owner",503,1,false,false); ready = true;
  transfer_sync::released = false; run("owner",local,"owner",503,1,false,false);
  transfer_sync::released = true;
  WifiCreds::fail = true; run("owner",local,"owner",401,0,false,false);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "handler.cpp"
            binary = Path(directory) / "handler"
            code.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "src/net"),
                            str(code), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
if __name__ == "__main__": unittest.main()
