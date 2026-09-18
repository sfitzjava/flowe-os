#include "../src/net/HttpResponseBatch.h"

#include <array>
#include <cassert>
#include <string>
#include <vector>

struct Sink {
  std::vector<std::string> writes;
  bool shortWrite = false;
  static size_t write(void* context, const uint8_t* data, size_t length) {
    auto& sink = *static_cast<Sink*>(context);
    sink.writes.emplace_back(reinterpret_cast<const char*>(data), length);
    return sink.shortWrite ? length - 1 : length;
  }
};

int main() {
  for (bool chunked : {false, true}) {
    for (size_t length : {size_t(0), size_t(1), size_t(1399), size_t(1400), size_t(1401), size_t(9000)}) {
      std::array<uint8_t, HttpResponseBatch::kStorageSize + 2> storage;
      storage.fill(0xa5);
      Sink sink;
      HttpResponseBatch batch(storage.data() + 1, chunked, Sink::write, &sink);
      std::string body(length, 'x');
      // Simulate short JSON rows, including splits across batch boundaries.
      for (size_t at = 0; at < body.size(); at += 107) {
        const size_t n = body.size() - at < 107 ? body.size() - at : 107;
        assert(batch.append(body.data() + at, n));
      }
      assert(batch.flush());
      assert(batch.flush());
      assert(sink.writes.size() == (length + 1399) / 1400);
      std::string decoded;
      for (const auto& frame : sink.writes) {
        if (!chunked) { decoded += frame; continue; }
        const auto headerEnd = frame.find("\r\n");
        const auto payload = std::stoul(frame.substr(0, headerEnd), nullptr, 16);
        assert(frame.size() == headerEnd + 2 + payload + 2);
        assert(frame.substr(frame.size() - 2) == "\r\n");
        decoded += frame.substr(headerEnd + 2, payload);
      }
      assert(decoded == body);
      assert(storage.front() == 0xa5 && storage.back() == 0xa5);
    }
    std::array<uint8_t, HttpResponseBatch::kStorageSize> storage{};
    Sink sink;
    sink.shortWrite = true;
    HttpResponseBatch batch(storage.data(), chunked, Sink::write, &sink);
    std::string body(5000, 'z');
    assert(!batch.append(body.data(), body.size()));
    assert(!batch.append("more"));
    assert(!batch.flush());
    assert(sink.writes.size() == 1);  // Never continue after a refused batch.
  }
}
