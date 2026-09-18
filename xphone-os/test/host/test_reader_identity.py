"""Check cached local identity without caching a request authorization result."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "src/net/FileTransferServer.cpp"
HARNESS = r'''#include <cassert>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
static bool failHeaderAllocation = false;
static unsigned constructedHeaderKeys = 0;
class String {
 public:
  std::string bytes;
  String(const char* s) : bytes(s) {
    if (bytes == "X-Flowe-Reader-Id") {
      ++constructedHeaderKeys;
      if (failHeaderAllocation) bytes.clear();
    }
  }
  String(std::string s) : bytes(std::move(s)) {}
  size_t length() const { return bytes.size(); }
  bool operator==(const char* s) const { return std::strcmp(bytes.c_str(), s) == 0; }
};
struct WebServer {
  std::vector<std::pair<std::string,std::string>> headers;
  unsigned lookups = 0;
  String header(const String& key) {
    ++lookups;
    for (const auto& h:headers) {
      auto a=h.first,b=key.bytes;
      for(char& c:a)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      for(char& c:b)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if(a==b)return String(h.second);
    }
    return String("");
  }
};
namespace WifiCreds {
constexpr size_t kReaderIdSize = 13;
unsigned reads = 0;
bool fail = false;
bool readerId(char* out,size_t n) {
  ++reads;out[0]=0;
  if(fail)return false;
  assert(n>=13);memcpy(out,"F85B1B5BD180",13);return true;
}
}
struct FileTransferServer { WebServer* _server; bool targetReaderOk(); };
PRODUCTION_METHOD
int main(int argc,char**) {
  WebServer web;
  FileTransferServer server{&web};
  if(argc>1) {
    failHeaderAllocation=true;
    assert(!server.targetReaderOk()); // cannot mistake failed key construction for absence
    assert(web.lookups==0 && WifiCreds::reads==0);
    std::cout<<"PASS: header key allocation failure fails closed\n";
    return 0;
  }
  assert(server.targetReaderOk() && WifiCreds::reads==0); // absent
  web.headers={{"X-Flowe-Reader-Id",""}};
  assert(server.targetReaderOk() && WifiCreds::reads==0); // present empty = absent
  web.headers={{"x-fLoWe-rEaDeR-iD","F85B1B5BD180"}};
  WifiCreds::fail=true;
  assert(!server.targetReaderOk() && WifiCreds::reads==1);
  WifiCreds::fail=false;
  assert(server.targetReaderOk() && WifiCreds::reads==2); // failed local read can retry
  for(unsigned i=0;i<5;++i) assert(server.targetReaderOk());
  assert(WifiCreds::reads==2 && constructedHeaderKeys==1);
  web.headers={{"X-Flowe-Reader-Id","AAAAAAAAAAAA"}};
  assert(!server.targetReaderOk()); // next request does not inherit earlier match
  web.headers={{"X-Flowe-Reader-Id","f85b1b5bd180"}};
  assert(!server.targetReaderOk()); // value is case-sensitive as before
  web.headers={{"X-Flowe-Reader-Id","F85B1B5BD180 "}};
  assert(!server.targetReaderOk());
  web.headers={{"X-Flowe-Reader-Id",std::string("F85B1B5BD180\0x",14)}};
  assert(server.targetReaderOk()); // Existing WString == const char* uses strcmp; unchanged here.
  web.headers.clear(); assert(server.targetReaderOk());
  web.headers={{"X-Flowe-Reader-Id","F85B1B5BD180"}};
  assert(server.targetReaderOk()); // mismatch did not poison later match
  WebServer next;
  next.headers={{"X-Flowe-Reader-Id","BBBBBBBBBBBB"}};
  server._server=&next;
  assert(!server.targetReaderOk() && next.lookups==1);
  assert(WifiCreds::reads==2 && constructedHeaderKeys==1);
  assert(web.lookups==15);
  std::cout<<"PASS: absent, empty, match, mismatch, key case, value case, request changes, local failure retry, local ID caching, one key construction and one lookup per check\n";
}
'''

class ReaderIdentityTest(unittest.TestCase):
    def test_current_request_and_allocation_failure(self):
        source = SOURCE.read_text()
        start = source.index("bool FileTransferServer::targetReaderOk() {")
        end = source.index("\n\nbool FileTransferServer::begin()", start)
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "identity.cpp"
            exe = Path(tmp) / "identity"
            cpp.write_text(HARNESS.replace("PRODUCTION_METHOD", source[start:end]))
            subprocess.run(["c++", "-std=c++17", "-O1", "-g",
                            "-fsanitize=address,undefined", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            subprocess.run([str(exe), "header-fail"], check=True)

if __name__ == "__main__":
    unittest.main()
