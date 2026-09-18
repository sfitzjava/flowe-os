// Real statistics serializer and real BLE sender, with only hardware edges replaced.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <limits>
#include "Arduino.h"
#include "reader/ReadingStats.cpp"
#if __has_include("ble/ReaderProgressChunks.h")
#include "ble/ReaderProgressChunks.h"
#endif

uint32_t testMillis = 1000;
ClockStore CLOCK_STORE;
static size_t allocationLimit = std::numeric_limits<size_t>::max();
static size_t allocationCalls = 0;
void* operator new(size_t n) {
  ++allocationCalls;
  if (n > allocationLimit) throw std::bad_alloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static unsigned testMtu = 517;
static unsigned disconnectAt = 0;
static char frames[2048][560];
static unsigned frameCount = 0;
static bool connected = true;
static void xSemaphoreTake(int, int) {}
static void xSemaphoreGive(int) {}
constexpr int portMAX_DELAY = 0;
static unsigned ble_att_mtu(uint16_t) { return testMtu; }
struct Characteristic {
  char value[560]{};
  void setValue(const char* p) { setValue(reinterpret_cast<const uint8_t*>(p), std::strlen(p)); }
  void setValue(const uint8_t* p, size_t n) {
    assert(n < sizeof(value));
    assert(n <= testMtu - 3);
    assert(n <= 512);  // Arduino BLECharacteristic rejects larger attributes.
    std::memcpy(value, p, n); value[n] = 0;
  }
};
struct CompanionBleService {
  Characteristic characteristic;
  Characteristic* actionCharacteristic = &characteristic;
  int stateMutex = 0;
  uint16_t secConnHandle = 1;
  bool progressRequested = false;
  uint32_t progressRetryAtMs = 0;
  bool isConnected() const { return connected; }
  void notifyAction() {
    assert(connected && frameCount < 2048);
    std::strcpy(frames[frameCount++], characteristic.value);
    if (disconnectAt && frameCount == disconnectAt) connected = false;
  }
  void sendReaderProgress();
};

// PRODUCTION_METHOD

int main(int argc, char** argv) {
  assert(argc == 4);
  testMtu = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
  allocationLimit = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
  disconnectAt = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
  CLOCK_STORE.day = 20261231; CLOCK_STORE.firstSyncMs = 1;
  reader::s_loaded = true;
  for (unsigned i = 0; i < 64; ++i) {
    reader::s_store.days[i] = {reader::ymdFromSerial(reader::serialFromYmd(20260101) + i), 65535, 65535};
    reader::s_store.books[i] = {UINT32_MAX - i, UINT32_MAX, UINT32_MAX, 20261231, 20260101, 65535, 0};
  }
  CompanionBleService service;
  allocationCalls = 0;
  try { service.sendReaderProgress(); }
  catch (const std::bad_alloc&) {
    std::fprintf(stderr, "reader.progress required an allocation larger than %zu bytes\n", allocationLimit);
    return 3;
  }
  if (testMtu == 23) { assert(frameCount == 0 && service.progressRequested); }
  else if (disconnectAt) { assert(frameCount == disconnectAt); }
  else { assert(frameCount > 0); }
  std::fprintf(stderr, "frames=%u serialization_allocations=%zu retry=%d\n", frameCount, allocationCalls, service.progressRequested);
  for (unsigned i = 0; i < frameCount; ++i) std::puts(frames[i]);
}
