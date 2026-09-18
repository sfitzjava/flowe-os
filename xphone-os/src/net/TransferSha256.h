#pragma once
#include <cstddef>
#include <cstdint>
#include <mbedtls/sha256.h>

namespace flowe_resume {
// Shared file-content hash for upload verification and strong download
// validators. Each context belongs to one synchronous transfer operation.
class Sha256 {
 public:
  Sha256() { mbedtls_sha256_init(&context); }
  ~Sha256() { mbedtls_sha256_free(&context); }
  bool reset() { return mbedtls_sha256_starts(&context, 0) == 0; }
  bool update(const uint8_t* data, size_t size) { return mbedtls_sha256_update(&context, data, size) == 0; }
  bool finish(char* hex) {
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&context, digest)) return false;
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) { hex[i * 2] = digits[digest[i] >> 4]; hex[i * 2 + 1] = digits[digest[i] & 15]; }
    hex[64] = 0;
    return true;
  }
 private:
  mbedtls_sha256_context context;
};
} // namespace flowe_resume
