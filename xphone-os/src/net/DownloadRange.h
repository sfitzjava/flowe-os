#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace flowe_download {
struct Plan { unsigned status; uint64_t first, length, total; };
struct Text { const char* bytes; size_t length; };
inline Text trim(Text text) {
  while (text.length && (text.bytes[0] == ' ' || text.bytes[0] == '\t')) { ++text.bytes; --text.length; }
  while (text.length && (text.bytes[text.length - 1] == ' ' || text.bytes[text.length - 1] == '\t')) --text.length;
  return text;
}
inline bool number(Text text, uint64_t& value) {
  if (!text.length) return false;
  value = 0;
  for (size_t i = 0; i < text.length; ++i) {
    const char c = text.bytes[i];
    if (c < '0' || c > '9' || value > (UINT64_MAX - uint64_t(c - '0')) / 10) return false;
    value = value * 10 + uint64_t(c - '0');
  }
  return true;
}

// RFC 9110: support one byte range; ignore malformed, multi-range, and
// unknown units. A false If-Range ignores Range entirely, including an
// otherwise unsatisfiable interval. Only our exact strong ETag can match.
inline Plan plan(uint64_t total, Text range, Text ifRange, const char* etag) {
  const Plan full{200, 0, total, total};
  range = trim(range); ifRange = trim(ifRange);
  if (!range.length) return full;
  if (ifRange.length && (ifRange.length != strlen(etag) || memcmp(ifRange.bytes, etag, ifRange.length))) return full;
  if (range.length < 7) return full;
  const char unit[] = "bytes=";
  for (size_t i = 0; i < sizeof(unit) - 1; ++i) {
    char c = range.bytes[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != unit[i]) return full;
  }
  Text value = trim({range.bytes + 6, range.length - 6});
  const char* dash = static_cast<const char*>(memchr(value.bytes, '-', value.length));
  if (!dash) return full;
  const size_t leftSize = size_t(dash - value.bytes);
  const Text left{value.bytes, leftSize}, right{dash + 1, value.length - leftSize - 1};
  uint64_t first = 0, last = 0;
  const Plan unsatisfied{416, 0, 0, total};
  if (!left.length) {
    uint64_t suffix = 0;
    if (!number(right, suffix)) return full;
    if (!suffix || !total) return unsatisfied;
    const uint64_t length = suffix < total ? suffix : total;
    return {206, total - length, length, total};
  }
  if (!number(left, first)) return full;
  if (right.length && (!number(right, last) || last < first)) return full;
  if (first >= total) return unsatisfied;
  if (!right.length || last >= total) last = total - 1;
  return {206, first, last - first + 1, total};
}

// Both hashing and response streaming use this exact bound. A short read
// is allowed; EOF before the promised length is an error. Never read or
// send bytes after a requested closed range, even when more file remains.
template<class File>
int readBounded(File& file, uint8_t* buffer, size_t capacity, uint64_t& remaining) {
  if (!remaining) return 0;
  if (!buffer || !capacity) return -1;
  const size_t count = remaining < capacity ? size_t(remaining) : capacity;
  const int result = file.read(buffer, count);
  if (result <= 0 || size_t(result) > count) return -1;
  remaining -= uint64_t(result);
  return result;
}

enum class HashResult { ok, io, cancelled };
// Hash the same open file handle that will supply the response. The reader
// has one main-loop SD writer; no other request can mutate it in this
// interval. No mtime/size-only cache can mistake a same-size replacement.
template<class File, class Hash, class Cancel, class Tick>
HashResult makeETag(File& file, Hash& hash, uint64_t expected, uint8_t* buffer,
                    size_t capacity, char (&etag)[67], Cancel cancel, Tick tick) {
  etag[0] = 0;
  if (!hash.reset() || !file.seekSet(0) || uint64_t(file.fileSize()) != expected) return HashResult::io;
  uint64_t remaining = expected;
  while (remaining) {
    if (cancel()) return HashResult::cancelled;
    const int read = readBounded(file, buffer, capacity, remaining);
    if (read <= 0 || !hash.update(buffer, size_t(read))) return HashResult::io;
    tick();
  }
  if (cancel()) return HashResult::cancelled;
  char digest[65];
  if (uint64_t(file.fileSize()) != expected || !hash.finish(digest) || !file.seekSet(0)) return HashResult::io;
  etag[0] = '"'; memcpy(etag + 1, digest, 64); etag[65] = '"'; etag[66] = 0;
  return HashResult::ok;
}
} // namespace flowe_download
