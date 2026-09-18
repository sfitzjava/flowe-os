#pragma once
#include <cstddef>
#include <cstdint>

namespace flowe_status {
struct Text { const char* bytes; size_t length; };
struct Authorization {
  bool allowed;
  bool prepareMemory;
  bool sessionVerified;
};
inline bool equal(Text a, Text b) {
  if (a.length != b.length) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < a.length; ++i) difference |= uint8_t(a.bytes[i]) ^ uint8_t(b.bytes[i]);
  return difference == 0;
}
// A public discovery response must never stand in for paired readiness.
// Length-aware comparisons reject NUL suffixes accepted by legacy String
// comparisons, and a guest server cannot accept a phone's nonempty token.
inline Authorization authorize(Text suppliedToken, Text suppliedReader,
                                Text sessionToken, Text localReader, bool ownedReadiness = true) {
  const Authorization denied{false, false, false};
  const bool exactReader = suppliedReader.length == 12 && localReader.length == 12 && equal(suppliedReader, localReader);
  if (suppliedReader.length && !exactReader) return denied;
  if (suppliedToken.length) {
    if (!sessionToken.length || !equal(suppliedToken, sessionToken)) return denied;
    if (!exactReader && ownedReadiness) return denied;
    // Old Android sends the BLE-issued token without a reader header.
    // Preparation is compatible; modern paired readiness still needs the ID.
    return {true, true, exactReader};
  }
  // Token-free status is public. Guest sessions retain their preparation barrier.
  // Old iOS probes publicly. Only BLE-negotiated legacy sessions retain
  // that preparation path. Strict sessions never prepare on public traffic.
  return {true, sessionToken.length == 0 || !ownedReadiness, false};
}
} // namespace flowe_status
