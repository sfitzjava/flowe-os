#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Borrow request scratch storage. The caller owns HTTP headers and the final
// chunk terminator; each payload batch and its framing use one socket write.
class HttpResponseBatch {
 public:
  static constexpr size_t kPayloadSize = 1400;
  static constexpr size_t kStorageSize = kPayloadSize + 10;
  using Write = size_t (*)(void*, const uint8_t*, size_t);

  HttpResponseBatch(uint8_t* storage, bool chunked, Write write, void* context)
      : _storage(storage), _chunked(chunked), _write(write), _context(context) {}

  bool append(const char* bytes, size_t length) {
    if (!_alive) return false;
    while (length) {
      const size_t space = kPayloadSize - _used;
      const size_t take = length < space ? length : space;
      memcpy(_storage + 8 + _used, bytes, take);
      _used += take;
      bytes += take;
      length -= take;
      if (_used == kPayloadSize && !flush()) return false;
    }
    return true;
  }

  bool append(const char* text) { return append(text, strlen(text)); }

  bool flush() {
    if (!_alive || !_used) return _alive;
    size_t start = 8, length = _used;
    if (_chunked) {
      char head[8];
      const size_t n = static_cast<size_t>(snprintf(head, sizeof(head), "%x\r\n",
                                                 static_cast<unsigned>(_used)));
      start -= n;
      memcpy(_storage + start, head, n);
      memcpy(_storage + 8 + _used, "\r\n", 2);
      length += n + 2;
    }
    _used = 0;
    _alive = _write(_context, _storage + start, length) == length;
    return _alive;
  }

 private:
  uint8_t* _storage;
  bool _chunked;
  Write _write;
  void* _context;
  size_t _used = 0;
  bool _alive = true;
};
