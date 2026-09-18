#pragma once
// SdFat rename does not replace an existing target (O_EXCL). Retain the
// prior file until promotion succeeds. The backup also survives a reset.
namespace flowe_upload {
// Complete an opened file exactly once. Even a failed flush must close it;
// no failed write/close may enter the publication path.
template<class Flush, class Close, class Publish>
bool finish(bool failed, Flush flush, Close close, Publish publish) {
  const bool flushed = flush();
  const bool closed = close();
  return !failed && flushed && closed && publish();
}
template<class Storage>
bool recover(Storage& sd, const char* final, const char* backup) {
  if (!sd.exists(backup)) return true;
  if (!sd.exists(final)) return sd.rename(backup, final);
  return sd.remove(backup);
}
template<class Storage>
bool publish(Storage& sd, const char* part, const char* final, const char* backup) {
  if (!recover(sd, final, backup)) return false;
  const bool prior = sd.exists(final);
  if (prior && !sd.rename(final, backup)) return false;
  if (!sd.rename(part, final)) {
    // A failed restore leaves the ONLY prior copy at backup for recovery.
    if (prior) sd.rename(backup, final);
    return false;
  }
  if (prior) sd.remove(backup); // Safe to recover this leftover next time.
  return true;
}
}
