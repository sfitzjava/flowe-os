#pragma once

// One durable upload slot. Storage and SHA-256 are adapters so the exact
// production transaction logic can run against fault-injected host storage.
// Metadata is written only after checked data sync. A reconnect starts at
// the persisted offset, never at a count observed in a socket callback.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace flowe_resume {
constexpr uint32_t kCheckpointBytes = 1024u * 1024u;
constexpr uint32_t kMaxBytes = 64u * 1024u * 1024u;
constexpr const char* kDataPath = "/.flowe-resume-data";
constexpr const char* kRecordPaths[2] = {"/.flowe-resume-0", "/.flowe-resume-1"};
enum class State : uint32_t { receiving = 1, publishing, complete, failed, cancelled };
enum class Result { ok, notFound, invalid, unauthorized, conflict, io, hashMismatch };

struct Record {
  uint32_t magic, version, sequence, state, size, offset;
  char readerId[13], path[192], id[33], secret[65], sha256[65];
  uint32_t checksum;
};
static_assert(sizeof(Record) == 396, "Resume record layout changed; version it");

inline bool hex(const char* text, size_t length) {
  if (!text || strlen(text) != length) return false;
  for (size_t i = 0; i < length; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  return true;
}
inline bool decimal(const char* text, uint32_t& value) {
  if (!text || !*text) return false;
  value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || value > (UINT32_MAX - uint32_t(*text - '0')) / 10) return false;
    value = value * 10 + uint32_t(*text - '0');
  }
  return true;
}
inline uint32_t checksum(const Record& record) {
  // CRC detects torn media writes. Authorization uses the independent
  // session token and 256-bit client secret, not this checksum.
  uint32_t crc = UINT32_MAX;
  auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  for (size_t i = 0; i < offsetof(Record, checksum); ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline bool safeTarget(const char* path) {
  if (!path || strncmp(path, "/books/", 7) || !path[7] || strlen(path) >= 192 ||
      strstr(path, "..") || strchr(path, '\\')) return false;
  for (const char* p = path; *p; ++p) {
    if (uint8_t(*p) < 32 || (*p == '/' && (p[1] == '.' || p[1] == '/' || !p[1]))) return false;
  }
  const char* name = strrchr(path, '/') + 1;
  return strlen(name) <= 96;
}
inline bool valid(const Record& r) {
  return r.magic == 0x46525531 && r.version == 1 && r.sequence != 0 &&
      r.state >= uint32_t(State::receiving) && r.state <= uint32_t(State::cancelled) &&
      r.size > 0 && r.size <= kMaxBytes && r.offset <= r.size &&
      r.readerId[12] == 0 && strlen(r.readerId) == 12 &&
      r.path[191] == 0 && r.id[32] == 0 && r.secret[64] == 0 && r.sha256[64] == 0 &&
      safeTarget(r.path) && hex(r.id, 32) && hex(r.secret, 64) && hex(r.sha256, 64) &&
      r.checksum == checksum(r);
}

template<class Storage, class Hash>
class Upload {
 public:
  explicit Upload(Storage& storage) : sd(storage) {}
  const Record& record() const { return current; }
  bool active() const { return writing; }

  // Loading is read-only. Bad credentials or offsets never trigger recovery
  // writes. A valid older metadata slot wins over a torn newer slot.
  Result load() {
    if (writing) return Result::conflict;
    Record a{}, b{};
    const bool va = sd.readRecord(0, a) && valid(a);
    const bool vb = sd.readRecord(1, b) && valid(b);
    if (!va && !vb) {
      current = {};
      return sd.recordExists(0) || sd.recordExists(1) ? Result::io : Result::notFound;
    }
    if (va && vb && a.sequence == b.sequence && memcmp(&a, &b, sizeof(a))) return Result::io;
    current = va && (!vb || a.sequence > b.sequence) ? a : b;
    return Result::ok;
  }
  Result authorize(const char* reader, const char* id, const char* secret) const {
    if (!current.sequence) return Result::notFound;
    if (!reader || !id || !secret || strcmp(reader, current.readerId) ||
        strcmp(id, current.id) || strcmp(secret, current.secret)) return Result::unauthorized;
    return Result::ok;
  }
  Result start(const char* reader, const char* path, uint32_t size, const char* digest,
               const char* id, const char* secret, bool initialBody = false) {
    if (!reader || strlen(reader) != 12 || !safeTarget(path) || !size || size > kMaxBytes ||
        !hex(digest, 64) || !hex(id, 32) || !hex(secret, 64)) return Result::invalid;
    const Result loaded = load();
    if (loaded != Result::ok && loaded != Result::notFound) return loaded;
    if (loaded == Result::ok && !strcmp(id, current.id)) {
      const Result auth = authorize(reader, id, secret);
      if (auth != Result::ok) return auth;
      if (size != current.size || strcmp(path, current.path) || strcmp(digest, current.sha256)) return Result::conflict;
      if (state() == State::cancelled) return Result::conflict;
      if (initialBody && (state() != State::receiving || current.offset != 0)) return Result::conflict;
      return recover();
    }
    if (loaded == Result::ok && state() != State::complete && state() != State::cancelled) return Result::conflict;
    Record next{};
    next.magic = 0x46525531; next.version = 1;
    next.sequence = current.sequence;
    next.state = uint32_t(State::receiving); next.size = size;
    strcpy(next.readerId, reader); strcpy(next.path, path); strcpy(next.id, id);
    strcpy(next.secret, secret); strcpy(next.sha256, digest);
    // The old slot is complete/cancelled or absent. Its final file is never
    // touched here, even if creation or metadata persistence fails.
    if (!sd.prepareTarget(path) || !sd.createData()) return Result::io;
    return save(next);
  }
  Result status(const char* reader, const char* id, const char* secret) {
    Result result = load();
    if (result != Result::ok) return result;
    result = authorize(reader, id, secret);
    return result == Result::ok ? recover() : result;
  }
  Result begin(const char* reader, const char* id, const char* secret, uint32_t offset, uint32_t length) {
    Result result = load();
    if (result != Result::ok) return result;
    result = authorize(reader, id, secret);
    if (result != Result::ok) return result;
    if (state() != State::receiving || offset != current.offset || !length || length > current.size - offset)
      return Result::conflict;
    result = recover();
    if (result != Result::ok) return result;
    // Rebuild SHA from the durable prefix on reconnect. Normal body writes
    // update the same context; no extra SD read occurs at each checkpoint.
    if (!hashFile(kDataPath, offset, false) || !sd.openData(offset)) return Result::io;
    position = offset; end = offset + length; writing = true;
    return Result::ok;
  }
  Result append(const uint8_t* data, size_t size) {
    if (!writing || !size || size > end - position) return Result::conflict;
    if (!sd.writeData(data, size) || !hash.update(data, size)) return failIO();
    position += uint32_t(size);
    if (position - current.offset >= kCheckpointBytes) return checkpoint();
    return Result::ok;
  }
  Result finish() {
    if (!writing || position != end) return Result::conflict;
    Result result = checkpoint();
    if (result != Result::ok) return result;
    writing = false;
    if (!sd.closeData()) return Result::io;
    if (position < current.size) return Result::ok;
    char digest[65];
    if (!hash.finish(digest)) return Result::io;
    Record next = current;
    if (strcmp(digest, current.sha256)) {
      next.state = uint32_t(State::failed);
      result = save(next);
      return result == Result::ok ? Result::hashMismatch : result;
    }
    // This record is durable before either publication rename. It lets
    // reset recovery distinguish the new final from an older good book.
    next.state = uint32_t(State::publishing);
    result = save(next);
    return result == Result::ok ? publish() : result;
  }
  void abort() {
    if (writing) sd.closeData();
    writing = false;
    // Buffered/uncommitted bytes may reach media, but are not acknowledged.
    // Recovery truncates that tail to the last synced metadata offset.
  }
  Result cancel(const char* reader, const char* id, const char* secret) {
    Result result = load();
    if (result != Result::ok) return result;
    result = authorize(reader, id, secret);
    if (result != Result::ok) return result;
    return discardLoaded();
  }
  // Privileged recovery for a fresh authenticated BLE session owner who
  // lost the old job secret (reinstall/another phone). The HTTP caller must
  // verify current nonempty session token, exact reader ID and explicit
  // discard intent. This never infers authority from stored metadata.
  Result reset() {
    const Result result = load();
    if (result == Result::notFound) return sd.removeData() ? Result::ok : Result::io;
    if (result != Result::ok) return result; // corrupt/torn-only metadata: retain all bytes
    return discardLoaded();
  }
  Result recover() {
    if (writing) return Result::conflict;
    if (state() == State::cancelled) return sd.removeData() ? Result::ok : Result::io;
    if (state() == State::failed) return Result::hashMismatch;
    if (state() == State::receiving) {
      uint32_t size = 0;
      if (!sd.fileSize(kDataPath, size) || size < current.offset) return Result::io;
      if (!sd.truncateData(current.offset)) return Result::io;
      // A reset/close failure can occur after the final data checkpoint but
      // before verification. There is no positive-length suffix to resend.
      // Re-read those durable bytes and finish the transaction on status.
      if (current.offset == current.size) {
        Record next = current;
        const Result verification = verifyFile(kDataPath);
        if (verification == Result::io) return verification;
        const bool verified = verification == Result::ok;
        next.state = uint32_t(verified ? State::publishing : State::failed);
        Result result = save(next);
        if (result != Result::ok) return result;
        return verified ? publish() : Result::hashMismatch;
      }
      return Result::ok;
    }
    if (state() == State::publishing) {
      if (!sd.recoverPublication()) return Result::io;
      if (sd.dataExists()) {
        const Result verification = verifyFile(kDataPath);
        if (verification != Result::ok) return verification;
        return publish();
      }
      const Result verification = verifyFile(current.path);
      if (verification != Result::ok) return verification;
      if (!sd.publicationComplete(current.path)) return Result::io;
      Record next = current; next.state = uint32_t(State::complete);
      return save(next);
    }
    // Receipts survive a reset, but must not claim a target subsequently
    // replaced or removed by a legacy client still contains these bytes.
    const Result verification = verifyFile(current.path);
    return verification == Result::hashMismatch ? Result::conflict : verification;
  }
  State state() const { return State(current.state); }

 private:
  Storage& sd;
  Hash hash;
  Record current{};
  uint32_t position = 0, end = 0;
  bool writing = false;
  Result discardLoaded() {
    if (state() == State::cancelled) return sd.removeData() ? Result::ok : Result::io;
    // Complete publication recovery first. Cancellation must not strand an
    // existing book at the publication backup or delete its only new copy.
    if (state() == State::publishing) {
      const Result result = recover();
      if (result != Result::ok) return result;
    }
    Record next = current; next.state = uint32_t(State::cancelled);
    const Result result = save(next); // tombstone BEFORE removing only our data file
    if (result != Result::ok) return result;
    return sd.removeData() ? Result::ok : Result::io;
  }
  Result save(Record next) {
    if (current.sequence == UINT32_MAX) return Result::io;
    next.sequence = current.sequence + 1;
    next.checksum = checksum(next);
    if (!sd.writeRecord(next.sequence & 1u, next)) return Result::io;
    current = next;
    return Result::ok;
  }
  Result failIO() { abort(); return Result::io; }
  Result checkpoint() {
    if (!sd.syncData()) return failIO();
    if (position == current.offset) return Result::ok;
    Record next = current; next.offset = position;
    Result result = save(next);
    if (result != Result::ok) abort();
    return result;
  }
  bool hashFile(const char* path, uint32_t count, bool exact) {
    if (!hash.reset()) return false;
    uint32_t size = 0;
    if (!sd.fileSize(path, size) || size < count || (exact && size != count) || !sd.openRead(path)) return false;
    uint8_t* buffer = sd.scratch();
    const size_t capacity = sd.scratchSize();
    bool ok = buffer && capacity;
    while (ok && count) {
      const size_t length = count < capacity ? count : capacity;
      ok = sd.read(buffer, length) && hash.update(buffer, length);
      count -= uint32_t(length);
      sd.feedWatchdog();
    }
    return sd.closeRead() && ok;
  }
  Result verifyFile(const char* path) {
    char digest[65];
    if (!hashFile(path, current.size, true) || !hash.finish(digest)) return Result::io;
    return strcmp(digest, current.sha256) ? Result::hashMismatch : Result::ok;
  }
  Result publish() {
    if (!sd.publish(kDataPath, current.path)) return Result::io;
    if (!sd.publicationComplete(current.path)) return Result::io;
    Record next = current; next.state = uint32_t(State::complete);
    return save(next);
  }
};
} // namespace flowe_resume
