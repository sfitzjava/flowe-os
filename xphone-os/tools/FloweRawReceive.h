#pragma once
#include <cstddef>
#include <cstdint>
#include <algorithm>

#if defined(FLOWE_SYNC_CONTROL)
namespace transfer_sync { bool pollControls(); }
#endif
namespace flowe_raw {
constexpr uint32_t maxBody = 64u * 1024u * 1024u;
inline bool parseLength(const char* text, size_t size, uint32_t& result) {
  result = 0;
  if (!size) return false;
  for (size_t i = 0; i < size; ++i) {
    const unsigned digit = static_cast<unsigned char>(text[i]) - '0';
    if (digit > 9 || result > (maxBody - digit) / 10u) return false;
    result = result * 10u + digit;
  }
  return result > 0;
}
struct Chunk { size_t size; const char* error; };
// peek: 0 = EOF, -1 = terminal error, 1 = data or temporary unavailability.
// It must not consume data. The production adapter uses MSG_PEEK|MSG_DONTWAIT.
template<class Client, class Clock, class Delay, class Peek>
Chunk receive(Client& client, uint8_t* buffer, size_t wanted, Clock now, Delay pause, Peek peek) {
  size_t received = 0;
  uint32_t lastProgress = now();
  while (received < wanted) {
#if defined(FLOWE_SYNC_CONTROL)
    if (transfer_sync::pollControls()) return {received, "cancelled"};
#endif
    if (client.fd() < 0) return {received, "handler-closed"};
    const int available = client.available();
    if (available > 0) {
      const size_t count = std::min(wanted - received, static_cast<size_t>(available));
      const int got = client.read(buffer + received, count);
      if (got < 0 || static_cast<size_t>(got) > count) return {received, "read-error"};
      if (got > 0) { received += got; lastProgress = now(); continue; }
    } else {
      if (!client.connected()) {
        if (client.available() > 0) continue;
        return {received, "disconnected"};
      }
      const int state = peek(client.fd());
      if (state <= 0) {
        if (client.available() > 0) continue;
        return {received, state == 0 ? "eof" : "socket-error"};
      }
    }
    if (static_cast<uint32_t>(now() - lastProgress) >= 5000u)
      return {received, "no-progress-timeout"};
    pause(2);
  }
  return {received, nullptr};
}
}
