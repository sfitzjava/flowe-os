#pragma once
#include <cstddef>
#include <cstdint>

namespace transfer_sync {
struct WriteResult { size_t bytes; bool complete; };
// Send callback: positive = sent bytes, 0 = would block, negative = closed.
// The timeout is the caller's existing socket timeout. No-progress waits
// yield and poll control input; partial writes never restart the body.
template<class Send, class Clock, class Pause, class Cancel>
WriteResult writeControlled(const uint8_t* bytes, size_t size, uint32_t timeoutMs,
                            Send send, Clock now, Pause pause, Cancel cancel) {
  size_t sent = 0;
  uint32_t lastProgress = now();
  while (sent < size) {
    if (cancel()) return {sent, false};
    const int n = send(bytes + sent, size - sent);
    if (n < 0 || static_cast<size_t>(n) > size - sent) return {sent, false};
    if (n > 0) { sent += n; lastProgress = now(); continue; }
    if (static_cast<uint32_t>(now() - lastProgress) >= timeoutMs) return {sent, false};
    pause(2);
  }
  return {sent, true};
}
}
