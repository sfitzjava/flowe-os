"""Exercise the production scratch-growth block with a failing host allocator."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "src/net/FileTransferServer.cpp"

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <iostream>
static std::map<void*,size_t> allocations;
static bool failAllocation = false;
static unsigned attempts = 0, releases = 0;
void* testMalloc(size_t size) {
  ++attempts;
  if(failAllocation) return nullptr;
  void* p = std::malloc(size); assert(p); allocations[p] = size; return p;
}
void testFree(void* p) {
  assert(allocations.count(p)); allocations.erase(p); ++releases; std::free(p);
}
struct SerialType { template<class... Args> void printf(const char*,Args...) {} } Serial;
struct EspType { unsigned getFreeHeap() const {return 50000;} } ESP;
namespace transfer_sync { bool released = false; bool memoryReleased(){return released;} }
namespace BoardConfig { enum class Board { XteinkX3, XteinkX4 }; struct Profile { Board board = Board::XteinkX4; } ACTIVE; }
struct Buffer { uint8_t* buffer = nullptr; size_t bufferCapacity = 4096; bool bufferGrowthTried = false; };
struct Server {
  Buffer _upload;
  void grow() {
#define malloc testMalloc
#define free testFree
BLOCK
#undef malloc
#undef free
  }
};
int main() {
  Server s;
  s._upload.buffer = static_cast<uint8_t*>(testMalloc(4096));
  auto* original = s._upload.buffer;
  memset(original, 0x71, 4096);
  const auto baselineAttempts = attempts;
  s.grow();
  assert(attempts == baselineAttempts && !s._upload.bufferGrowthTried);
  assert(s._upload.buffer == original && s._upload.bufferCapacity == 4096);
  transfer_sync::released = true;
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX3;
  s.grow(); s.grow();
  assert(attempts == baselineAttempts && s._upload.buffer == original);
  assert(!s._upload.bufferGrowthTried && s._upload.bufferCapacity == 4096);
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4;
  s.grow();
  assert(s._upload.bufferGrowthTried && s._upload.bufferCapacity == 16384);
  assert(s._upload.buffer != original && !allocations.count(original));
  assert(allocations.size() == 1 && allocations[s._upload.buffer] == 16384);
  memset(s._upload.buffer, 0x52, s._upload.bufferCapacity); // entire selected capacity is valid
  const auto afterGrowth = attempts;
  s.grow(); s.grow(); assert(attempts == afterGrowth);
  testFree(s._upload.buffer); s._upload = {};

  // A new session can fall back without losing ownership or existing bytes.
  s._upload.buffer = static_cast<uint8_t*>(testMalloc(4096));
  original = s._upload.buffer;
  memset(original, 0x3a, 4096);
  const auto priorReleases = releases;
  failAllocation = true;
  s.grow();
  assert(s._upload.bufferGrowthTried && s._upload.bufferCapacity == 4096);
  assert(s._upload.buffer == original && releases == priorReleases);
  for(size_t i=0;i<4096;++i) assert(original[i] == 0x3a);
  const auto failedAttempts = attempts;
  failAllocation = false;
  s.grow(); assert(attempts == failedAttempts); // no allocation churn after failure
  testFree(s._upload.buffer); s._upload = {};
  assert(allocations.empty());
  // A later session gets a new attempt and can grow successfully.
  s._upload.buffer = static_cast<uint8_t*>(testMalloc(4096));
  s.grow(); assert(s._upload.bufferCapacity == 16384);
  testFree(s._upload.buffer);
  assert(allocations.empty());
  std::cout << "PASS: release gate, grow ownership, full capacity, one attempt, allocation failure retention, no retry churn, session reset, cleanup\n";
}
'''

class TransferScratchTest(unittest.TestCase):
    def test_growth_lifecycle(self):
        source = SOURCE.read_text()
        start = source.index("    if (!_upload.bufferGrowthTried && transfer_sync::memoryReleased() &&")
        end = source.index("\n#if defined(FLOWE_BENCH_UPLOAD_PROFILE)", start)
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "growth.cpp"
            exe = Path(tmp) / "growth"
            cpp.write_text(HARNESS.replace("BLOCK", source[start:end]))
            subprocess.run(["c++", "-std=c++17", "-O1", "-g",
                            "-fsanitize=address,undefined", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    unittest.main()
