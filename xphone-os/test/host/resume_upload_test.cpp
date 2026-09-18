#include "ResumeUpload.h"
#include "UploadPublication.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace flowe_resume;

// Deterministic hash adapter; the transaction tests do not re-test mbedTLS.
// It is sensitive to both byte values and order and emits the wire hex size.
struct Hash {
  uint64_t value = 1469598103934665603ull;
  bool reset() { value = 1469598103934665603ull; return true; }
  bool update(const uint8_t* data, size_t size) {
    while (size--) { value ^= *data++; value *= 1099511628211ull; }
    return true;
  }
  bool finish(char* text) {
    for (unsigned i = 0; i < 4; ++i) snprintf(text + i * 16, 17, "%016llx", (unsigned long long)(value + i));
    return true;
  }
};
using Bytes = std::vector<uint8_t>;
struct Storage {
  std::map<std::string, Bytes> files;
  std::array<uint8_t, 4096> buffer{};
  size_t readPosition = 0, writePosition = 0, reads = 0, mutations = 0, syncs = 0;
  std::string readPath;
  bool writer = false, failWrite = false, failSync = false, failClose = false, failRecord = false;
  bool failTruncate = false, failSideEffects = false, failRead = false;
  uint32_t failRecordState = 0;
  int publishFault = 0;
  uint8_t* scratch() { return buffer.data(); }
  size_t scratchSize() { return buffer.size(); }
  void feedWatchdog() {}
  bool exists(const char* path) { return files.count(path); }
  bool remove(const char* path) { ++mutations; files.erase(path); return true; }
  bool rename(const char* source, const char* target) {
    ++mutations;
    if (!exists(source) || exists(target)) return false;
    files[target] = files[source]; files.erase(source); return true;
  }
  bool recordExists(unsigned slot) { return exists(kRecordPaths[slot]); }
  bool readRecord(unsigned slot, Record& record) {
    const auto it = files.find(kRecordPaths[slot]);
    if (it == files.end() || it->second.size() != sizeof(record)) return false;
    memcpy(&record, it->second.data(), sizeof(record)); return true;
  }
  bool writeRecord(unsigned slot, const Record& record) {
    ++mutations;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    const bool failed = failRecord || record.state == failRecordState;
    files[kRecordPaths[slot]] = Bytes(bytes, bytes + (failed ? sizeof(record) / 2 : sizeof(record)));
    return !failed;
  }
  bool prepareTarget(const char*) { return true; }
  bool createData() { ++mutations; files[kDataPath] = {}; return true; }
  bool dataExists() { return exists(kDataPath); }
  bool removeData() { return !dataExists() || remove(kDataPath); }
  bool fileSize(const char* path, uint32_t& length) {
    if (!exists(path)) return false;
    length = uint32_t(files[path].size()); return true;
  }
  bool truncateData(uint32_t offset) {
    if (failTruncate || !dataExists() || files[kDataPath].size() < offset) return false;
    ++mutations; files[kDataPath].resize(offset); return true;
  }
  bool openData(uint32_t offset) {
    if (!dataExists() || files[kDataPath].size() != offset) return false;
    writer = true; writePosition = offset; return true;
  }
  bool writeData(const uint8_t* data, size_t size) {
    assert(writer); ++mutations;
    const size_t written = failWrite ? size / 2 : size;
    auto& file = files[kDataPath]; file.resize(writePosition + written);
    memcpy(file.data() + writePosition, data, written); writePosition += written;
    return !failWrite;
  }
  bool syncData() { ++syncs; return !failSync; }
  bool closeData() { writer = false; return !failClose; }
  bool openRead(const char* path) { readPath = path; readPosition = 0; return exists(path); }
  bool read(uint8_t* data, size_t size) {
    if (failRead || readPosition + size > files[readPath].size()) return false;
    memcpy(data, files[readPath].data() + readPosition, size); readPosition += size; reads += size; return true;
  }
  bool closeRead() { return true; }
  static constexpr const char* backup = "/.old";
  static constexpr const char* journal = "/.target";
  bool recoverPublication() {
    if (!exists(backup)) { if (exists(journal)) remove(journal); return true; }
    if (!exists(journal)) return false;
    auto& bytes = files[journal];
    std::string path(bytes.begin(), bytes.end());
    if (!flowe_upload::recover(*this, path.c_str(), backup)) return false;
    remove(journal); return true;
  }
  bool publish(const char* source, const char* target) {
    if (!recoverPublication()) return false;
    files[journal] = Bytes(target, target + strlen(target));
    if (publishFault == 1) return false; // power loss before first rename
    if (publishFault == 2 || publishFault == 3) {
      if (exists(target)) assert(rename(target, backup));
      if (publishFault == 3) assert(rename(source, target));
      return false; // power loss after backup or promotion
    }
    const bool result = flowe_upload::publish(*this, source, target, backup);
    if (!exists(backup)) remove(journal);
    return result;
  }
  bool publicationComplete(const char*) { return !failSideEffects; }
};
using Engine = Upload<Storage, Hash>;
static constexpr const char* reader = "F85B1BFC2E18";
static constexpr const char* id = "0123456789abcdef0123456789abcdef";
static constexpr const char* secret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static constexpr const char* target = "/books/test.fbp";
std::string digest(const Bytes& bytes) {
  Hash hash; hash.reset(); hash.update(bytes.data(), bytes.size());
  char result[65]; hash.finish(result); return result;
}
Bytes payload(size_t length) {
  Bytes bytes(length);
  for (size_t i = 0; i < length; ++i) bytes[i] = uint8_t(i * 17 + (i >> 8));
  return bytes;
}
void start(Engine& engine, const Bytes& bytes) {
  assert(engine.start(reader, target, uint32_t(bytes.size()), digest(bytes).c_str(), id, secret) == Result::ok);
}
void send(Engine& engine, const Bytes& bytes, size_t offset, size_t size, size_t chunk = 1436) {
  assert(engine.begin(reader, id, secret, uint32_t(offset), uint32_t(size)) == Result::ok);
  size_t end = offset + size;
  for (; offset < end;) {
    const size_t count = std::min(chunk, end - offset);
    assert(engine.append(bytes.data() + offset, count) == Result::ok); offset += count;
  }
}
void characterization() {
  uint32_t n;
  assert(decimal("0", n) && n == 0);
  assert(decimal("4294967295", n) && n == UINT32_MAX);
  for (auto invalid : {"", "-1", "4294967296", "1x", " 1"}) assert(!decimal(invalid, n));
  assert(safeTarget("/books/sub/book.fbp"));
  for (auto path : {"/books/../bad", "/.flowe-resume-data", "/books/.hidden", "/books/x/", "/books//x", "/books/x\\y", "/books/x\ny"}) assert(!safeTarget(path));
  Storage sd; Engine engine(sd); auto bytes = payload(12345); start(engine, bytes);
  Record record = engine.record(); assert(valid(record));
  record.offset++; assert(!valid(record));
  const size_t mutations = sd.mutations;
  assert(engine.start(reader, target, bytes.size(), digest(bytes).c_str(), id, secret) == Result::ok);
  // Idempotent start performs recovery, but does not remove or replace a target.
  assert(sd.mutations >= mutations && engine.record().offset == 0);
  puts("resume: validation and idempotent start passed");
}
void identity_and_offset() {
  Storage sd; Engine engine(sd); auto bytes = payload(kCheckpointBytes * 2 + 19); start(engine, bytes);
  send(engine, bytes, 0, bytes.size(), kCheckpointBytes);
  assert(engine.finish() == Result::ok);
  assert(sd.files[target] == bytes);
  const size_t mutations = sd.mutations;
  assert(engine.status(reader, id, "bad") == Result::unauthorized);
  assert(engine.cancel("000000000000", id, secret) == Result::unauthorized);
  assert(engine.start(reader, target, bytes.size() - 1, digest(bytes).c_str(), id, secret) == Result::conflict);
  assert(engine.begin(reader, id, secret, bytes.size(), 1) == Result::conflict);
  assert(sd.mutations == mutations && sd.files[target] == bytes);
  assert(engine.status(reader, id, secret) == Result::ok && engine.state() == State::complete);
  sd.files[target][0] ^= 1;
  assert(engine.status(reader, id, secret) == Result::conflict); // never a stale receipt
  puts("resume: identity, complete receipt, and stale final passed");
}
void checkpoint_and_reconnect() {
  Storage sd; Engine engine(sd); auto bytes = payload(kCheckpointBytes * 3 + 55); start(engine, bytes);
  assert(engine.begin(reader, id, secret, 0, bytes.size()) == Result::ok);
  assert(engine.append(bytes.data(), kCheckpointBytes - 1) == Result::ok);
  assert(engine.record().offset == 0);
  assert(engine.append(bytes.data() + kCheckpointBytes - 1, 1) == Result::ok);
  assert(engine.record().offset == kCheckpointBytes && sd.syncs == 1);
  assert(sd.reads == 0); // checkpoints never re-read the growing file
  assert(engine.append(bytes.data() + kCheckpointBytes, 9999) == Result::ok);
  engine.abort();
  const size_t mutations = sd.mutations;
  assert(engine.begin(reader, id, secret, kCheckpointBytes + 1, 1) == Result::conflict);
  assert(engine.begin("000000000000", id, secret, kCheckpointBytes, 1) == Result::unauthorized);
  assert(sd.mutations == mutations && sd.files[kDataPath].size() == kCheckpointBytes + 9999);
  Engine reboot(sd);
  assert(reboot.status(reader, id, secret) == Result::ok);
  assert(reboot.record().offset == kCheckpointBytes && sd.files[kDataPath].size() == kCheckpointBytes);
  send(reboot, bytes, kCheckpointBytes, bytes.size() - kCheckpointBytes);
  assert(sd.reads == kCheckpointBytes); // exactly one prefix hash read
  assert(reboot.finish() == Result::ok && sd.files[target] == bytes);
  puts("resume: checkpoint boundary, exact offsets, prefix hash, tail truncation passed");
}
void torn_record() {
  Storage sd; Engine engine(sd); auto bytes = payload(kCheckpointBytes * 3); start(engine, bytes);
  assert(engine.begin(reader, id, secret, 0, bytes.size()) == Result::ok);
  assert(engine.append(bytes.data(), kCheckpointBytes) == Result::ok);
  sd.failRecord = true;
  assert(engine.append(bytes.data() + kCheckpointBytes, kCheckpointBytes) == Result::io);
  assert(!engine.active()); sd.failRecord = false;
  Engine reboot(sd); assert(reboot.status(reader, id, secret) == Result::ok);
  assert(reboot.record().offset == kCheckpointBytes && sd.files[kDataPath].size() == kCheckpointBytes);
  send(reboot, bytes, kCheckpointBytes, bytes.size() - kCheckpointBytes);
  assert(reboot.finish() == Result::ok && sd.files[target] == bytes);
  // Both corrupt slots fail closed; a fresh start cannot delete unknown bytes.
  sd.files[kRecordPaths[0]] = {0}; sd.files[kRecordPaths[1]] = {0};
  auto prior = sd.files;
  assert(reboot.start(reader, target, bytes.size(), digest(bytes).c_str(), id, secret) == Result::io);
  assert(sd.files == prior);
  puts("resume: torn checkpoint and corrupt metadata fail closed passed");
}
void failed_io() {
  for (int fault = 0; fault < 4; ++fault) {
    Storage sd; Engine engine(sd); auto bytes = payload(kCheckpointBytes + 40); start(engine, bytes);
    assert(engine.begin(reader, id, secret, 0, bytes.size()) == Result::ok);
    sd.failWrite = fault == 0; sd.failSync = fault == 1; sd.failRecord = fault == 2;
    if (fault < 3) assert(engine.append(bytes.data(), kCheckpointBytes) == Result::io);
    else {
      assert(engine.append(bytes.data(), bytes.size()) == Result::ok);
      sd.failClose = true; assert(engine.finish() == Result::io);
    }
    assert(!sd.exists(target));
    sd.failWrite = sd.failSync = sd.failRecord = sd.failClose = false;
    Engine reboot(sd);
    if (fault == 2) {
      // Initial record is intact despite the torn alternate slot.
      assert(reboot.status(reader, id, secret) == Result::ok);
    } else assert(reboot.status(reader, id, secret) == Result::ok);
    const uint32_t offset = reboot.record().offset;
    if (offset == bytes.size()) {
      // Close failed after a durable full checkpoint. Retry completion by
      // idempotent start/status must finish verification and publication.
      assert(reboot.state() == State::complete);
    } else {
      send(reboot, bytes, offset, bytes.size() - offset);
      assert(reboot.finish() == Result::ok);
    }
    assert(sd.files[target] == bytes);
  }
  puts("resume: data write, sync, metadata, and close faults passed");
}
void hash_and_cancel() {
  Storage sd; Engine engine(sd); auto bytes = payload(65432); start(engine, bytes);
  const Bytes original = {1, 2, 3}; sd.files[target] = original;
  auto wrong = bytes; wrong[50] ^= 9;
  send(engine, wrong, 0, wrong.size());
  assert(engine.finish() == Result::hashMismatch);
  assert(engine.state() == State::failed && sd.files[target] == original);
  assert(engine.cancel(reader, id, secret) == Result::ok);
  assert(!sd.dataExists() && sd.files[target] == original);
  assert(engine.cancel(reader, id, secret) == Result::ok); // repeated cancel
  assert(engine.start(reader, target, bytes.size(), digest(bytes).c_str(), id, secret) == Result::conflict);
  const char* nextId = "1123456789abcdef0123456789abcdef";
  assert(engine.start(reader, target, bytes.size(), digest(bytes).c_str(), nextId, secret) == Result::ok);
  // A torn cancel cannot delete the live data or invalidate its last record.
  sd.failRecord = true;
  assert(engine.cancel(reader, nextId, secret) == Result::io && sd.dataExists());
  sd.failRecord = false;
  Engine reboot(sd); assert(reboot.status(reader, nextId, secret) == Result::ok);
  assert(reboot.state() == State::receiving && sd.files[target] == original);
  puts("resume: checksum mismatch, old-book preservation, durable cancel passed");
}
void publication_recovery() {
  for (int fault = 1; fault <= 5; ++fault) {
    Storage sd; Engine engine(sd); auto bytes = payload(77777); start(engine, bytes);
    const Bytes original = {11, 22, 33}; sd.files[target] = original;
    send(engine, bytes, 0, bytes.size());
    sd.publishFault = fault <= 3 ? fault : 0;
    sd.failSideEffects = fault == 4;
    if (fault == 5) {
      sd.failRecordState = uint32_t(State::complete); // torn complete receipt
    }
    assert(engine.finish() == Result::io);
    auto equals = [&](const char* path, const Bytes& value) {
      const auto it = sd.files.find(path); return it != sd.files.end() && it->second == value;
    };
    assert(equals(target, original) || equals(Storage::backup, original) || equals(target, bytes));
    sd.publishFault = 0; sd.failSideEffects = false; sd.failRecordState = 0;
    Engine reboot(sd);
    auto recovered = reboot.status(reader, id, secret);
    if (recovered != Result::ok) fprintf(stderr, "publication fault=%d result=%d\n", fault, int(recovered));
    assert(recovered == Result::ok);
    assert(reboot.state() == State::complete && sd.files[target] == bytes);
    assert(!sd.dataExists() && !sd.exists(Storage::backup));
    assert(reboot.cancel(reader, id, secret) == Result::ok && sd.files[target] == bytes);
  }
  puts("resume: publication reset points and completed cancellation passed");
}
void implicit_start_and_recovery_io() {
  Storage sd; Engine engine(sd); auto bytes = payload(kCheckpointBytes + 25);
  const std::string sha = digest(bytes);
  assert(engine.start(reader, target, bytes.size(), sha.c_str(), id, secret, true) == Result::ok);
  assert(engine.begin(reader, id, secret, 0, bytes.size()) == Result::ok);
  assert(engine.append(bytes.data(), kCheckpointBytes) == Result::ok);
  assert(engine.append(bytes.data() + kCheckpointBytes, 10) == Result::ok);
  engine.abort();
  const auto prior = sd.files;
  assert(engine.start(reader, target, bytes.size(), sha.c_str(), id, secret, true) == Result::conflict);
  assert(sd.files == prior); // rejected offset-0 retry must not truncate
  const char* nextId = "1123456789abcdef0123456789abcdef";
  assert(engine.start(reader, target, bytes.size(), sha.c_str(), nextId, secret, true) == Result::conflict);
  assert(sd.files == prior); // queued next book cannot displace an incomplete book
  sd.failTruncate = true;
  assert(engine.status(reader, id, secret) == Result::io);
  sd.failTruncate = false;
  send(engine, bytes, kCheckpointBytes, 25);
  sd.failClose = true;
  assert(engine.finish() == Result::io);
  sd.failClose = false; sd.failRead = true;
  Engine reboot(sd);
  assert(reboot.status(reader, id, secret) == Result::io);
  assert(reboot.state() == State::receiving); // transient read fault is not corrupt content
  sd.failRead = false;
  assert(reboot.status(reader, id, secret) == Result::ok && reboot.state() == State::complete);
  assert(reboot.start(reader, target, bytes.size(), sha.c_str(), id, secret, true) == Result::conflict);
  assert(reboot.start(reader, "/books/next.fbp", bytes.size(), sha.c_str(), nextId, secret, true) == Result::ok);
  assert(sd.files[target] == bytes && reboot.record().offset == 0);
  puts("resume: implicit cohort start, failed retry guards, recovery I/O passed");
}
void owner_reset() {
  const char* nextId = "1123456789abcdef0123456789abcdef";
  auto bytes = payload(kCheckpointBytes + 29);
  const std::string sha = digest(bytes);
  Storage absent; Engine empty(absent);
  absent.files["/books/keep.epub"] = {1, 2, 3};
  const auto original = absent.files;
  assert(empty.reset() == Result::ok && empty.reset() == Result::ok);
  assert(absent.files == original);
  absent.files[kDataPath] = {4, 5}; // failed initial start before either record existed
  assert(empty.reset() == Result::ok && absent.files == original);
  Storage sd; Engine engine(sd); start(engine, bytes);
  sd.files[target] = {3, 2, 1}; const auto oldFinal = sd.files[target];
  assert(engine.begin(reader, id, secret, 0, bytes.size()) == Result::ok);
  assert(engine.reset() == Result::conflict && engine.active());
  assert(engine.append(bytes.data(), kCheckpointBytes) == Result::ok);
  engine.abort();
  assert(engine.cancel(reader, id, "lost-secret") == Result::unauthorized);
  assert(engine.reset() == Result::ok && !sd.dataExists() && sd.files[target] == oldFinal);
  assert(engine.start(reader, target, bytes.size(), sha.c_str(), nextId, secret) == Result::ok);
  assert(engine.begin(reader, nextId, secret, 0, bytes.size()) == Result::ok);
  assert(engine.append(bytes.data(), bytes.size()) == Result::ok);
  assert(engine.finish() == Result::ok);
  assert(engine.reset() == Result::ok && sd.files[target] == bytes);
  for (int fault = 1; fault <= 3; ++fault) {
    Storage interrupted; Engine pending(interrupted); start(pending, bytes);
    interrupted.files[target] = oldFinal;
    send(pending, bytes, 0, bytes.size());
    interrupted.publishFault = fault;
    assert(pending.finish() == Result::io);
    interrupted.publishFault = 0;
    Engine reboot(interrupted);
    assert(reboot.reset() == Result::ok);
    assert(interrupted.files[target] == bytes && !interrupted.dataExists() && !interrupted.exists(Storage::backup));
    assert(reboot.state() == State::cancelled);
  }
  // Unknown/corrupt metadata never authorizes deletion or publication.
  Storage corrupt; Engine broken(corrupt);
  corrupt.files[kRecordPaths[0]] = {5, 6};
  corrupt.files[kDataPath] = {7, 8}; corrupt.files[target] = {9, 10};
  const auto untouched = corrupt.files;
  assert(broken.reset() == Result::io && corrupt.files == untouched);
  Storage torn; Engine active(torn); start(active, bytes);
  torn.failRecord = true;
  assert(active.reset() == Result::io && torn.dataExists());
  torn.failRecord = false;
  Engine recovered(torn); assert(recovered.reset() == Result::ok && !torn.dataExists());
  puts("resume: privileged orphan reset, receipt/old final, publication recovery, torn reset passed");
}
int main() {
  characterization(); identity_and_offset(); checkpoint_and_reconnect(); torn_record();
  failed_io(); hash_and_cancel(); publication_recovery(); implicit_start_and_recovery_io(); owner_reset();
  puts("resume upload production state-machine tests passed");
}
