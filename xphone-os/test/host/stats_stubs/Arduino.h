#pragma once
#include <cstddef>
#include <cstdint>
extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
inline void delay(unsigned ms) { testMillis += ms; }
struct TestSerial {
  template <class... Args> void printf(const char*, Args...) {}
  void println(const char*) {}
};
inline TestSerial Serial;
