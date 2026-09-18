#pragma once
#include "ResumeUpload.h"
#include <SDCardManager.h>
#include <esp_task_wdt.h>
#include "TransferSha256.h"

namespace flowe_resume {
// Reuses FileTransferServer's reserved transfer buffer. No new payload
// allocation, worker task, SD concurrency, or on-device format is needed.
class SdStorage {
 public:
  bool (*recoverPublication)() = nullptr;
  bool (*publish)(const char*, const char*) = nullptr;
  bool (*publicationComplete)(const char*) = nullptr;
  void configure(uint8_t* scratch, size_t capacity) { buffer = scratch; bufferSize = capacity; }
  uint8_t* scratch() { return buffer; }
  size_t scratchSize() const { return bufferSize; }
  void feedWatchdog() { esp_task_wdt_reset(); }
  bool recordExists(unsigned slot) { return SdMan.exists(kRecordPaths[slot]); }
  bool readRecord(unsigned slot, Record& record) {
    FsFile file = SdMan.open(kRecordPaths[slot], O_RDONLY);
    if (!file) return false;
    const bool read = file.size() == sizeof(record) && file.read(&record, sizeof(record)) == int(sizeof(record));
    return file.close() && read;
  }
  bool writeRecord(unsigned slot, const Record& record) {
    FsFile file = SdMan.open(kRecordPaths[slot], O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    const bool written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    const bool synced = file.sync();
    const bool closed = file.close();
    return written && synced && closed;
  }
  bool prepareTarget(const char* path) {
    char directory[192];
    snprintf(directory, sizeof(directory), "%s", path);
    *strrchr(directory, '/') = 0;
    return SdMan.exists(directory) || SdMan.mkdir(directory);
  }
  bool createData() {
    FsFile file = SdMan.open(kDataPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    const bool synced = file.sync();
    return file.close() && synced;
  }
  bool dataExists() { return SdMan.exists(kDataPath); }
  bool removeData() { return !dataExists() || SdMan.remove(kDataPath); }
  bool fileSize(const char* path, uint32_t& size) {
    FsFile file = SdMan.open(path, O_RDONLY);
    if (!file) return false;
    const uint64_t length = file.size();
    size = uint32_t(length);
    return file.close() && length <= UINT32_MAX;
  }
  bool truncateData(uint32_t offset) {
    FsFile file = SdMan.open(kDataPath, O_RDWR);
    if (!file) return false;
    const bool truncated = file.size() >= offset && (file.size() == offset || file.truncate(offset));
    const bool synced = file.sync();
    return file.close() && truncated && synced;
  }
  bool openData(uint32_t offset) {
    buffered = 0;
    writer = SdMan.open(kDataPath, O_RDWR);
    if (!writer) return false;
    if (writer.size() != offset || !writer.seekSet(offset)) { writer.close(); return false; }
    return true;
  }
  bool writeData(const uint8_t* bytes, size_t size) {
    if (!writer || !buffer || !bufferSize) return false;
    while (size) {
      const size_t available = bufferSize - buffered;
      const size_t count = size < available ? size : available;
      memcpy(buffer + buffered, bytes, count);
      buffered += count; bytes += count; size -= count;
      if (buffered == bufferSize && !flush()) return false;
    }
    return true;
  }
  bool syncData() { return writer && flush() && writer.sync(); }
  bool closeData() {
    if (!writer) return true;
    const bool synced = syncData();
    const bool closed = writer.close();
    buffered = 0;
    return synced && closed;
  }
  bool openRead(const char* path) { reader = SdMan.open(path, O_RDONLY); return bool(reader); }
  bool read(uint8_t* bytes, size_t size) { return reader.read(bytes, size) == int(size); }
  bool closeRead() { return reader.close(); }
 private:
  FsFile writer, reader;
  uint8_t* buffer = nullptr;
  size_t bufferSize = 0, buffered = 0;
  bool flush() {
    if (!buffered) return true;
    const bool written = writer.write(buffer, buffered) == buffered;
    buffered = 0; // never duplicate a short write on close
    feedWatchdog();
    return written;
  }
};
} // namespace flowe_resume
