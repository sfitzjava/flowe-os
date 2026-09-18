#pragma once

#if defined(FLOWE_BENCH_SD_WRITE)
#include <Arduino.h>
#include <BoardConfig.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <cstdlib>
#include <cstring>
#include "BenchFramebufferLoan.h"
#include "Scene.h"
#include "StallWatch.h"
#include "scenes/AppScenes.h"

namespace bench {
inline uint8_t sdWritePattern(const size_t offset) {
  const size_t i = offset & 511u;
  return static_cast<uint8_t>(i * 37u + (i >> 8) * 101u + 23u);
}

inline void sdWriteFeedWatchdogs() {
  esp_task_wdt_reset();
  feedLoopWDT();
  stallwatch::beat();
  yield();
}

// Only three fixed tests. A stale file is evidence to inspect, never ours
// to overwrite or remove. All SD work stays on the main loop task.
inline bool handleSdWriteCommand(const char* command) {
  const bool preallocate = !strcmp(command, "sdwriteprealloc");
  const bool large = !strcmp(command, "sdwrite16384");
  if (!preallocate && !large && strcmp(command, "sdwrite4096")) return false;
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX4) {
    Serial.println("[sdwrite] refused: X4 only");
    return true;
  }
#if defined(FLOWE_BENCH_FRAMEBUFFER_LOAN)
  if (framebufferLoaned()) {
    Serial.println("[sdwrite] refused: framebuffer experiment active");
    return true;
  }
#endif
  if (gCurrentSceneId == SceneId::FileTransfer || WiFi.getMode() != WIFI_OFF) {
    Serial.println("[sdwrite] refused: leave transfer and stop Wi-Fi first");
    return true;
  }
  if (!SdMan.ready()) {
    Serial.println("[sdwrite] refused: SD not ready");
    return true;
  }
  SCENES.waitFlushIdle();  // display and SD share SPI; exclude panel work
  constexpr char kPath[] = "/books/.Speed0911-sdwrite.tmp";
  constexpr size_t kBytes = 8u * 1024u * 1024u;
  const size_t block = large ? 16384u : 4096u;
  uint8_t* const buffer = static_cast<uint8_t*>(malloc(block));
  if (!buffer) {
    Serial.printf("[sdwrite] refused: cannot allocate %u bytes\n", static_cast<unsigned>(block));
    return true;
  }
  for (size_t i = 0; i < block; ++i) buffer[i] = sdWritePattern(i);
  sdWriteFeedWatchdogs();
  const uint32_t createAt = micros();
  FsFile file = SdMan.open(kPath, O_WRONLY | O_CREAT | O_EXCL);
  const uint32_t createUs = micros() - createAt;
  if (!file) {
    free(buffer);
    Serial.println("[sdwrite] refused: exclusive create failed; existing file left untouched");
    return true;
  }
  // From here onward this invocation owns the newly created path. Every
  // error reaches close and cleanup; no early return may leave it open.
  const uint32_t preallocateAt = micros();
  const bool preallocateOk = !preallocate || file.preAllocate(kBytes);
  const uint32_t preallocateUs = preallocate ? micros() - preallocateAt : 0;
  size_t written = 0;
  uint32_t maxWriteUs = 0;
  bool writeOk = preallocateOk;
  sdWriteFeedWatchdogs();
  const uint32_t cpuBefore = getCpuFrequencyMhz();
  const uint32_t writeAt = micros();
  while (writeOk && written < kBytes) {
    const uint32_t blockAt = micros();
    const size_t count = file.write(buffer, block);
    const uint32_t blockUs = micros() - blockAt;
    if (blockUs > maxWriteUs) maxWriteUs = blockUs;
    written += count;
    writeOk = count == block;
    sdWriteFeedWatchdogs();
  }
  const bool syncOk = file.sync();
  sdWriteFeedWatchdogs();
  const bool writeCloseOk = file.close();
  const uint32_t writeUs = micros() - writeAt;
  const uint32_t cpuAfter = getCpuFrequencyMhz();
  const bool complete = preallocateOk && writeOk && written == kBytes && syncOk && writeCloseOk;

  size_t verified = 0;
  bool verifyOk = false;
  bool readCloseOk = true;
  const uint32_t verifyAt = micros();
  if (complete) {
    file = SdMan.open(kPath, O_RDONLY);
    if (file) {
      verifyOk = file.fileSize() == kBytes;
      while (verifyOk && verified < kBytes) {
        const int count = file.read(buffer, block);
        if (count != static_cast<int>(block)) {
          verifyOk = false;
          break;
        }
        for (size_t i = 0; i < block; ++i) {
          if (buffer[i] != sdWritePattern(verified + i)) {
            Serial.printf("[sdwrite] verification mismatch at byte %u\n",
                          static_cast<unsigned>(verified + i));
            verifyOk = false;
            break;
          }
        }
        if (verifyOk) verified += block;
        sdWriteFeedWatchdogs();
      }
      readCloseOk = file.close();
      verifyOk = verifyOk && readCloseOk && verified == kBytes;
    }
  }
  const uint32_t verifyUs = micros() - verifyAt;
  sdWriteFeedWatchdogs();
  const bool removed = SdMan.remove(kPath);
  const bool cleanupOk = removed && !SdMan.exists(kPath);
  free(buffer);
  Serial.printf("[sdwrite] cpuMHz=%lu->%lu (power policy unchanged)\n",
                static_cast<unsigned long>(cpuBefore), static_cast<unsigned long>(cpuAfter));
  Serial.printf("[sdwrite] block=%u preallocate=%u target=%u written=%u createUs=%lu preallocateUs=%lu writeSyncCloseUs=%lu maxWriteUs=%lu verifyUs=%lu\n",
                static_cast<unsigned>(block), preallocate ? 1u : 0u, static_cast<unsigned>(kBytes),
                static_cast<unsigned>(written), static_cast<unsigned long>(createUs),
                static_cast<unsigned long>(preallocateUs), static_cast<unsigned long>(writeUs),
                static_cast<unsigned long>(maxWriteUs), static_cast<unsigned long>(verifyUs));
  Serial.printf("[sdwrite] preallocateOk=%u writeOk=%u syncOk=%u writeCloseOk=%u verified=%u verifyOk=%u readCloseOk=%u cleanupOk=%u result=%s\n",
                preallocateOk ? 1u : 0u, writeOk ? 1u : 0u, syncOk ? 1u : 0u,
                writeCloseOk ? 1u : 0u, static_cast<unsigned>(verified), verifyOk ? 1u : 0u,
                readCloseOk ? 1u : 0u, cleanupOk ? 1u : 0u,
                complete && verifyOk && cleanupOk ? "ok" : "failed");
  sdWriteFeedWatchdogs();
  return true;
}
}  // namespace bench
#endif
