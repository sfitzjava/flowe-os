#pragma once
#include <cstddef>
struct HalFile {
  size_t read(void*, size_t) { return 0; }
  size_t write(const void*, size_t n) { return n; }
  void flush() {}
};
struct TestStorage {
  bool openFileForRead(const char*, const char*, HalFile&) { return false; }
  bool openFileForWrite(const char*, const char*, HalFile&) { return true; }
  bool remove(const char*) { return true; }
  bool rename(const char*, const char*) { return true; }
};
inline TestStorage Storage;
