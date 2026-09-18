#include "DownloadRange.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using namespace flowe_download;
const char* tag = "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"";
Text text(const char* value) { return {value, strlen(value)}; }
Plan choose(uint64_t total, const char* range, const char* condition = "") {
  return plan(total, text(range), text(condition), tag);
}
void expect(uint64_t total, const char* range, unsigned status, uint64_t first, uint64_t length, const char* condition = "") {
  const Plan actual = choose(total, range, condition);
  assert(actual.status == status && actual.first == first && actual.length == length && actual.total == total);
}
void plans() {
  expect(100, "", 200, 0, 100);
  expect(100, "bytes=0-0", 206, 0, 1);
  expect(100, "bytes=20-29", 206, 20, 10, tag);
  expect(100, "bytes=20-", 206, 20, 80, tag);
  expect(100, "bytes=99-", 206, 99, 1);
  expect(100, "bytes=20-1000", 206, 20, 80);
  expect(100, "bytes=-20", 206, 80, 20);
  expect(100, "bytes=-1000", 206, 0, 100);
  expect(100, "bytes=-0", 416, 0, 0);
  expect(100, "bytes=100-", 416, 0, 0);
  expect(100, "bytes=18446744073709551615-", 416, 0, 0);
  expect(0, "", 200, 0, 0);
  expect(0, "bytes=0-0", 416, 0, 0);
  expect(0, "bytes=-1", 416, 0, 0);
  expect(100, " \tBYTES=20-29\t ", 206, 20, 10);
  for (const char* range : {"items=0-5", "bytes=", "bytes=-", "bytes=5-2", "bytes=0-1,5-6", "bytes=0- 5", "bytes=+1-2", "bytes=18446744073709551616-", "bytes=0-18446744073709551616", "bytes=1-2x"}) expect(100, range, 200, 0, 100);
  for (const char* condition : {"W/\"old\"", "\"old\"", "Mon, 01 Jan 2024 00:00:00 GMT", "*", "broken"}) {
    expect(100, "bytes=20-", 200, 0, 100, condition);
    expect(100, "bytes=100-", 200, 0, 100, condition); // If-Range evaluated before unsatisfiable
  }
  const std::string weak = std::string("W/") + tag;
  expect(100, "bytes=20-", 200, 0, 100, weak.c_str());
  const char nulRange[] = "bytes=0-1\0,2-5";
  assert(plan(100, {nulRange, sizeof(nulRange) - 1}, text(""), tag).status == 200);
  const std::string nulTag = std::string(tag) + std::string("\0x", 2);
  assert(plan(100, text("bytes=5-"), {nulTag.data(), nulTag.size()}, tag).status == 200);
  // Exhaustively check all small closed intervals, including empty sources.
  for (uint64_t size = 0; size < 64; ++size) for (uint64_t a = 0; a < 70; ++a) for (uint64_t b = a; b < 70; ++b) {
    const std::string request = "bytes=" + std::to_string(a) + "-" + std::to_string(b);
    const auto selected = choose(size, request.c_str());
    if (a >= size) assert(selected.status == 416 && selected.length == 0);
    else assert(selected.status == 206 && selected.first == a && selected.length == std::min(b, size - 1) - a + 1);
  }
  puts("download range: interval planning, If-Range, overflow, malformed headers passed");
}
struct File {
  std::vector<uint8_t> bytes;
  size_t position = 0, requested = 0, largestRead = 0, readCalls = 0, sizeCalls = 0;
  size_t maxRead = SIZE_MAX;
  bool failSeek = false, failRead = false, growAfterHash = false, overreport = false;
  explicit File(size_t count) : bytes(count) { for (size_t i = 0; i < count; ++i) bytes[i] = uint8_t(i * 17 + i / 11); }
  uint64_t fileSize() { return bytes.size() + (growAfterHash && ++sizeCalls > 1 ? 1 : 0); }
  bool seekSet(uint64_t offset) { if (failSeek || offset > bytes.size()) return false; position = size_t(offset); return true; }
  int read(uint8_t* out, size_t count) {
    ++readCalls; requested += count; largestRead = std::max(largestRead, count);
    if (failRead) return -1;
    size_t got = std::min({count, maxRead, bytes.size() - position});
    memcpy(out, bytes.data() + position, got); position += got;
    return overreport ? int(count + 1) : int(got);
  }
};
// The production mbedTLS adapter is shared with upload verification. This
// hash adapter checks streaming/order and same-size changes without adding
// a host dependency on the firmware's crypto library.
struct Hash {
  uint64_t value = 0; size_t count = 0;
  bool failReset = false, failUpdate = false, failFinish = false;
  bool reset() { value = 1469598103934665603ull; count = 0; return !failReset; }
  bool update(const uint8_t* bytes, size_t size) {
    count += size;
    while (size--) { value ^= *bytes++; value *= 1099511628211ull; }
    return !failUpdate;
  }
  bool finish(char* hex) {
    for (int i = 0; i < 4; ++i) snprintf(hex + i * 16, 17, "%016llx", (unsigned long long)(value + i));
    return !failFinish;
  }
};
void bounded_body() {
  uint8_t buffer[4096];
  File file(25000); assert(file.seekSet(101));
  uint64_t remaining = 10003;
  std::vector<uint8_t> response;
  while (remaining) {
    const int got = readBounded(file, buffer, sizeof(buffer), remaining);
    assert(got > 0); response.insert(response.end(), buffer, buffer + got);
  }
  assert(file.position == 101 + 10003 && response == std::vector<uint8_t>(file.bytes.begin() + 101, file.bytes.begin() + 101 + 10003));
  assert(file.requested == 10003 && file.largestRead <= sizeof(buffer));
  const size_t calls = file.readCalls;
  assert(readBounded(file, buffer, sizeof(buffer), remaining) == 0 && file.readCalls == calls);
  file.maxRead = 3; file.seekSet(0); remaining = 17;
  while (remaining) assert(readBounded(file, buffer, sizeof(buffer), remaining) > 0);
  assert(file.position == 17);
  remaining = 5; assert(readBounded(file, nullptr, 0, remaining) == -1 && remaining == 5);
  file.seekSet(file.bytes.size()); assert(readBounded(file, buffer, sizeof(buffer), remaining) == -1 && remaining == 5);
  file.seekSet(0); file.overreport = true;
  assert(readBounded(file, buffer, sizeof(buffer), remaining) == -1 && remaining == 5);
  puts("download range: bounded reads, short reads, EOF and invalid reads passed");
}
void etags() {
  uint8_t buffer[4096]; char oldTag[67], newTag[67];
  File file(65539); Hash hash;
  size_t ticks = 0;
  auto cancel = [] { return false; };
  assert(makeETag(file, hash, file.bytes.size(), buffer, sizeof(buffer), oldTag, cancel, [&] { ++ticks; }) == HashResult::ok);
  assert(hash.count == file.bytes.size() && file.position == 0 && file.requested == file.bytes.size());
  assert(file.largestRead <= sizeof(buffer) && ticks == 17 && strlen(oldTag) == 66 && oldTag[0] == '"' && oldTag[65] == '"');
  assert(plan(file.bytes.size(), text("bytes=100-"), text(oldTag), oldTag).status == 206);
  file.bytes[5000] ^= 1; // same name, size and handle; different content
  assert(makeETag(file, hash, file.bytes.size(), buffer, sizeof(buffer), newTag, cancel, [] {}) == HashResult::ok);
  assert(strcmp(oldTag, newTag));
  auto replacement = plan(file.bytes.size(), text("bytes=100-"), text(oldTag), newTag);
  assert(replacement.status == 200 && replacement.first == 0 && replacement.length == file.bytes.size());
  File empty(0);
  assert(makeETag(empty, hash, 0, buffer, sizeof(buffer), newTag, cancel, [] {}) == HashResult::ok && empty.readCalls == 0);
  for (int fault = 0; fault < 7; ++fault) {
    File broken(10003); Hash badHash;
    broken.failSeek = fault == 0; broken.failRead = fault == 1; broken.growAfterHash = fault == 2;
    badHash.failReset = fault == 3; badHash.failUpdate = fault == 4; badHash.failFinish = fault == 5;
    if (fault == 6) broken.bytes.resize(100);
    newTag[0] = 'x';
    assert(makeETag(broken, badHash, 10003, buffer, sizeof(buffer), newTag, cancel, [] {}) == HashResult::io);
    assert(newTag[0] == 0); // never expose a partial validator
  }
  File interrupted(10003); size_t polls = 0;
  assert(makeETag(interrupted, hash, interrupted.bytes.size(), buffer, sizeof(buffer), newTag,
      [&] { return ++polls >= 2; }, [] {}) == HashResult::cancelled);
  assert(interrupted.position == sizeof(buffer) && newTag[0] == 0);
  puts("download range: fresh validators, same-size replacement, hash faults and cancel passed");
}
int main() { plans(); bounded_body(); etags(); puts("download range production helper tests passed"); }
