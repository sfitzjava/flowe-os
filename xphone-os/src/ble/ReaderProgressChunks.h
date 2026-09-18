#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Stream the ASCII ReadingStats JSON into the existing BLE envelope. Storage
// is constant even when all 64 day and book records are populated. The final
// frame is held until finish() verifies the count; a refused or changed report
// must never claim completion on the phone.
class ReaderProgressChunks {
 public:
  using Sink = bool (*)(const char*, size_t, void*);
  ReaderProgressChunks(size_t total, size_t frameBudget, uint32_t token, Sink sink, void* context)
      : _total(total), _budget(frameBudget < sizeof(_json) - 1 ? frameBudget : sizeof(_json) - 1),
        _token(token), _sink(sink), _context(context) {
    _ok = total <= 65535 && _budget >= 124 && sink;
    if (_ok) beginFrame();
  }

  bool append(const char* bytes, size_t length) {
    if (!_ok || _finished || length > _total - _received) return _ok = false;
    for (size_t i = 0; i < length; ++i) {
      const unsigned char c = static_cast<unsigned char>(bytes[i]);
      char escaped[7];
      size_t n = 1;
      escaped[0] = static_cast<char>(c);
      if (c == '"' || c == '\\') { escaped[0] = '\\'; escaped[1] = static_cast<char>(c); n = 2; }
      else if (c < 0x20) { std::snprintf(escaped, sizeof(escaped), "\\u%04x", c); n = 6; }
      if (_used + n + kSuffixBudget > _budget) {
        if (!_frameBytes || !flush(false)) return _ok = false;
      }
      if (_used + n + kSuffixBudget > _budget) return _ok = false;
      std::memcpy(_json + _used, escaped, n);
      _used += n;
      ++_received;
      ++_frameBytes;
    }
    return true;
  }

  bool finish() {
    if (!_ok || _finished || _received != _total) return _ok = false;
    return flush(true);
  }
  unsigned frames() const { return _seq; }

 private:
  static constexpr size_t kSuffixBudget = sizeof("\",\"done\":false}") - 1;
  // GATT attributes stop at 512 bytes even when ATT MTU is 517 or larger.
  char _json[513];
  size_t _total, _budget, _received = 0, _used = 0, _frameBytes = 0;
  uint32_t _token;
  unsigned _seq = 0;
  Sink _sink;
  void* _context;
  bool _ok = false, _finished = false;

  void beginFrame() {
    const int n = std::snprintf(_json, sizeof(_json),
        "{\"schemaVersion\":1,\"type\":\"reader.progress\",\"tok\":%lu,\"seq\":%u,\"len\":%u,\"d\":\"",
        static_cast<unsigned long>(_token), _seq, static_cast<unsigned>(_total));
    _ok = n > 0 && static_cast<size_t>(n) + kSuffixBudget < _budget;
    _used = _ok ? static_cast<size_t>(n) : 0;
    _frameBytes = 0;
  }
  bool flush(bool done) {
    if (!_ok) return false;
    const int n = std::snprintf(_json + _used, sizeof(_json) - _used,
                              "\",\"done\":%s}", done ? "true" : "false");
    if (n <= 0 || _used + static_cast<size_t>(n) > _budget ||
        !_sink(_json, _used + static_cast<size_t>(n), _context)) return _ok = false;
    ++_seq;
    _finished = done;
    if (!done) beginFrame();
    return _ok;
  }
};
