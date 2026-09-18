#include "AllocationProbe.h"
#if defined(FLOWE_ALLOCATION_PROBE)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_private/cache_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <lwip/mem.h>

namespace {
struct Event {
  uint32_t time, size, caps, free, taskHandle, caller, pointer, phase, frames;
  char task[16];
};
Event lows[8], failures[4];
uintptr_t framePointers[64] = {};
volatile uint32_t frameCount = 0;
uint32_t framePeak = 0, frameOverflow = 0;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool enabled = false;
uint32_t lowCount = 0, failedCount = 0, calls = 0, minimum = UINT32_MAX, phase = 0;

// No printing, dynamic storage, or filesystem calls in this path. Skipping
// cache-off and ISR calls is explicit: this is a scoped diagnostic, not a
// complete heap census. Wrappers stay in IRAM so normal allocation still works.
void IRAM_ATTR record(void* ptr, size_t size, uint32_t caps, uintptr_t caller, bool failed = false) {
  if (!enabled || !spi_flash_cache_enabled() || xPortInIsrContext()) return;
  const uint32_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  taskENTER_CRITICAL(&lock);
  ++calls;
  // Count only the observed 2312-byte frame allocation. Nested wrappers must
  // not count one allocation twice. This is a size cohort, not all Wi-Fi RAM.
  if (ptr && size == 2312) {
    unsigned slot = 64;
    bool found = false;
    for (unsigned i = 0; i < 64; ++i) {
      if (framePointers[i] == (uintptr_t)ptr) { found = true; break; }
      if (!framePointers[i]) slot = i;
    }
    if (!found && slot < 64) {
      framePointers[slot] = (uintptr_t)ptr;
      if (++frameCount > framePeak) framePeak = frameCount;
    } else if (!found) ++frameOverflow;
  }
  const bool lower = free < minimum;
  const bool outer = lowCount && ptr && lows[(lowCount-1)%8].pointer == (uintptr_t)ptr &&
                     lows[(lowCount-1)%8].free == free;
  const bool failedOuter = !ptr && size && failedCount &&
    failures[(failedCount-1)%4].size == size &&
    failures[(failedCount-1)%4].taskHandle == (uintptr_t)xTaskGetCurrentTaskHandle();
  if (lower || outer || failed || failedOuter) {
    Event* e;
    if (failed) e = &failures[(failedCount++)%4];
    else if (failedOuter) e = &failures[(failedCount-1)%4];
    else if (lower) { minimum = free; e = &lows[(lowCount++)%8]; }
    else e = &lows[(lowCount-1)%8];
    e->time = xTaskGetTickCount(); e->size = size; e->caps = caps; e->free = free;
    e->taskHandle = (uintptr_t)xTaskGetCurrentTaskHandle(); e->caller = caller; e->pointer = (uintptr_t)ptr; e->phase = phase; e->frames = frameCount;
    const char* name = pcTaskGetName(nullptr);
    unsigned i = 0;
    for (; i < sizeof(e->task)-1 && name[i]; ++i) e->task[i] = name[i];
    e->task[i] = 0;
  }
  taskEXIT_CRITICAL(&lock);
}
void IRAM_ATTR releaseFrame(void* ptr) {
  if (!enabled || !ptr || !spi_flash_cache_enabled() || xPortInIsrContext()) return;
  taskENTER_CRITICAL(&lock);
  for (unsigned i = 0; i < 64; ++i) {
    if (framePointers[i] == (uintptr_t)ptr) {
      framePointers[i] = 0; --frameCount; break;
    }
  }
  taskEXIT_CRITICAL(&lock);
}
void failedAlloc(size_t size, uint32_t caps, const char*) {
  record(nullptr, size, caps, (uintptr_t)__builtin_return_address(0), true);
}
void printEvent(const char* kind, unsigned number, const Event& e) {
  Serial.printf("[allocprobe] %s n=%u tick=%lu size=%lu caps=%08lx free=%lu pc=%08lx ptr=%08lx phase=%lu frames=%lu task=%s\n",
    kind, number, (unsigned long)e.time, (unsigned long)e.size, (unsigned long)e.caps,
    (unsigned long)e.free, (unsigned long)e.caller, (unsigned long)e.pointer,
    (unsigned long)e.phase, (unsigned long)e.frames, e.task);
}
}

void allocationProbeStart() {
  enabled = false;
  lowCount = failedCount = calls = 0; minimum = UINT32_MAX; phase = 0;
  heap_caps_register_failed_alloc_callback(failedAlloc);
  memset(framePointers, 0, sizeof(framePointers));
  frameCount = framePeak = frameOverflow = 0;
  enabled = true;
}
void allocationProbePhase(unsigned value) { phase = value; }
void allocationProbeFinish() {
  enabled = false;
  Serial.printf("[allocprobe] summary calls=%lu lows=%lu failures=%lu minObserved=%lu storage=%u frameLive=%lu framePeak=%lu frameOverflow=%lu\n",
    (unsigned long)calls, (unsigned long)lowCount, (unsigned long)failedCount,
    (unsigned long)minimum, (unsigned)(sizeof(lows)+sizeof(failures)+sizeof(framePointers)),
    (unsigned long)frameCount, (unsigned long)framePeak, (unsigned long)frameOverflow);
  for (unsigned i = lowCount > 8 ? lowCount-8 : 0; i < lowCount; ++i) printEvent("low", i, lows[i%8]);
  for (unsigned i = failedCount > 4 ? failedCount-4 : 0; i < failedCount; ++i) printEvent("failed", i, failures[i%4]);
}
__attribute__((noinline)) void allocationProbeSelfTest() {
  if (!enabled || ESP.getFreeHeap() < 10000) { Serial.println("[allocprobe] selftest requires active server and 10 KB free"); return; }
  const uint32_t before = calls;
  void* (*volatile allocate)(size_t) = malloc;
  void (*volatile deallocate)(void*) = free;
  taskENTER_CRITICAL(&lock);
  const uint32_t beforeFrames = frameCount;
  void* p = allocate(2312);
  const uint32_t duringFrames = frameCount;
  if (p) { static_cast<volatile uint8_t*>(p)[0] = 42; deallocate(p); }
  const uint32_t afterFrames = frameCount;
  taskEXIT_CRITICAL(&lock);
  Serial.printf("[allocprobe] selftest allocated=%u observedCalls=%lu frames=%lu/%lu/%lu ok=%u\n",
    p != nullptr, (unsigned long)(calls-before), (unsigned long)beforeFrames,
    (unsigned long)duringFrames, (unsigned long)afterFrames,
    p && duringFrames == beforeFrames+1 && afterFrames == beforeFrames);
}

#define PROBE_WRAP __attribute__((noinline, noipa)) IRAM_ATTR
extern "C" {
void* __real_mem_malloc(mem_size_t);
void __real_free(void*);
void __real_heap_caps_free(void*);
void* PROBE_WRAP __wrap_mem_malloc(mem_size_t n) { void* p=__real_mem_malloc(n); record(p,n,MALLOC_CAP_DEFAULT,(uintptr_t)__builtin_return_address(0)); return p; }
void PROBE_WRAP __wrap_free(void* p) { releaseFrame(p); __real_free(p); }
void PROBE_WRAP __wrap_heap_caps_free(void* p) { releaseFrame(p); __real_heap_caps_free(p); }
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_heap_caps_malloc(size_t, uint32_t);
void* __real_heap_caps_malloc_default(size_t);
void* __real_heap_caps_calloc(size_t, size_t, uint32_t);
void* __real_heap_caps_realloc(void*, size_t, uint32_t);
void* __real_heap_caps_malloc_base(size_t, uint32_t);
void* PROBE_WRAP __wrap_malloc(size_t n) { void* p=__real_malloc(n); record(p,n,MALLOC_CAP_DEFAULT,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_calloc(size_t n,size_t s) { void* p=__real_calloc(n,s); record(p,n*s,MALLOC_CAP_DEFAULT,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_realloc(void* old,size_t n) { void* p=__real_realloc(old,n); record(p,n,MALLOC_CAP_DEFAULT,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_heap_caps_malloc(size_t n,uint32_t c) { void* p=__real_heap_caps_malloc(n,c); record(p,n,c,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_heap_caps_malloc_default(size_t n) { void* p=__real_heap_caps_malloc_default(n); record(p,n,MALLOC_CAP_DEFAULT,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_heap_caps_calloc(size_t n,size_t s,uint32_t c) { void* p=__real_heap_caps_calloc(n,s,c); record(p,n*s,c,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_heap_caps_realloc(void* old,size_t n,uint32_t c) { void* p=__real_heap_caps_realloc(old,n,c); record(p,n,c,(uintptr_t)__builtin_return_address(0)); return p; }
void* PROBE_WRAP __wrap_heap_caps_malloc_base(size_t n,uint32_t c) { void* p=__real_heap_caps_malloc_base(n,c); record(p,n,c,(uintptr_t)__builtin_return_address(0)); return p; }
}
#endif
