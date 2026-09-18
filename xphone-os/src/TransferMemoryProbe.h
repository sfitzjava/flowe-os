#pragma once
#include <stdint.h>
#include "AllocationProbe.h"

// Bench-only measurements. No buffer allocations and no change to TCP/SD sizes.
// A local heap minimum covers server startup through teardown, not earlier boot.
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcp_priv.h>

inline bool gTransferMemoryLocal = false;

// Read TCP lists only on the TCP/IP task. The wait completes before the
// stack-owned result goes away. This diagnostic never closes a connection.
inline void transferNetworkMemoryProbe(const char* stage = "console") {
  struct Snapshot {
    uint32_t free = 0, largest = 0, minimum = 0;
    unsigned timeWait = 0, active = 0, listen = 0, queuedBytes = 0;
    unsigned states[11] = {};
    uint32_t dmaFree = 0, dmaLargest = 0;
    struct Peer { uint32_t ack, next, window, congestion; uint16_t port; uint8_t retries, state; } peers[4] = {};
    unsigned peerCount = 0;
  } snapshot;
  const err_t result = tcpip_callback_wait([](void* context) {
    auto& s = *static_cast<Snapshot*>(context);
    for (auto* p = tcp_tw_pcbs; p; p = p->next) ++s.timeWait;
    for (auto* p = tcp_listen_pcbs.listen_pcbs; p; p = p->next) ++s.listen;
    for (auto* p = tcp_active_pcbs; p; p = p->next) {
      ++s.active;
      if (s.peerCount < 4) {
        auto& peer = s.peers[s.peerCount++];
        peer = {p->lastack, p->snd_nxt, p->snd_wnd, p->cwnd, p->remote_port, p->nrtx, static_cast<uint8_t>(p->state)};
      }
      if (static_cast<unsigned>(p->state) < 11) ++s.states[p->state];
      for (auto* q = p->unsent; q; q = q->next) if (q->p) s.queuedBytes += q->p->tot_len;
      for (auto* q = p->unacked; q; q = q->next) if (q->p) s.queuedBytes += q->p->tot_len;
    }
    s.dmaFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s.dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s.free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s.largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s.minimum = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  }, &snapshot);
  Serial.printf("[netmem] t=%lu stage=%s rc=%d free=%lu min=%lu largest=%lu tw=%u pcbSize=%u active=%u listen=%u queued=%u states=",
                (unsigned long)millis(), stage, result, (unsigned long)snapshot.free,
                (unsigned long)snapshot.minimum, (unsigned long)snapshot.largest,
                snapshot.timeWait, (unsigned)sizeof(tcp_pcb), snapshot.active,
                snapshot.listen, snapshot.queuedBytes);
  for (unsigned i = 0; i < 11; ++i) Serial.printf("%s%u", i ? "," : "", snapshot.states[i]);
  Serial.printf(" dmaFree=%lu dmaLargest=%lu\n", (unsigned long)snapshot.dmaFree, (unsigned long)snapshot.dmaLargest);
  if (!strcmp(stage, "write-stall")) {
    for (unsigned i = 0; i < snapshot.peerCount; ++i) {
      const auto& p = snapshot.peers[i];
      Serial.printf("[netpeer] port=%u state=%u ack=%lu next=%lu window=%lu cwnd=%lu retries=%u\n", p.port, p.state,
          (unsigned long)p.ack, (unsigned long)p.next, (unsigned long)p.window, (unsigned long)p.congestion, p.retries);
    }
  }
}

inline void transferMemoryProbe(const char* stage, uint32_t bytes = 0) {
  const uint32_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const uint32_t minimum = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const uint32_t stack = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[memprobe] t=%lu stage=%s bytes=%lu free=%lu min=%lu largest=%lu stack=%lu cpu=%lu scope=%s\n",
                (unsigned long)millis(), stage, (unsigned long)bytes,
                (unsigned long)freeBytes, (unsigned long)minimum, (unsigned long)largest,
                (unsigned long)stack, (unsigned long)getCpuFrequencyMhz(),
                gTransferMemoryLocal ? "server" : "boot");
}

inline void transferMemoryStart() {
  allocationProbeStart();
  transferMemoryProbe("before-server-reservation");
  if (!gTransferMemoryLocal) {
    gTransferMemoryLocal = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
  }
}

inline void transferMemoryStop() {
  allocationProbeFinish();
  if (!gTransferMemoryLocal) return;
  transferMemoryProbe("after-server-release");
  heap_caps_monitor_local_minimum_free_size_stop();
  gTransferMemoryLocal = false;
}
#else
inline void transferMemoryProbe(const char*, uint32_t = 0) {}
inline void transferMemoryStart() {}
inline void transferMemoryStop() {}
#endif
