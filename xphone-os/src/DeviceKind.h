#pragma once

// Which Xteink this binary woke up on. Set ONCE at the top of boot() from
// freeink::selectXteinkDevice() (I2C fingerprint of the X3-only gauge/RTC/IMU)
// before any scene, service, or SD/display bring-up runs. false = X4, the
// SDK's conservative pre-detection default. One universal update.bin serves
// both devices — a compile-time panel choice bricked testers who SD-flashed
// the other device's image (both zips ship a file named update.bin).
extern bool gDeviceIsX3;

// Device-kind label for screens and protocols. Sticky is a compile-time
// single-device build (FREEINK_DEVICE_STICKY, platformio.ini [base-s3]);
// X3/X4 remain one runtime-detected universal binary.
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
inline const char* deviceKindUpper() { return "Sticky"; }
inline const char* deviceKindLower() { return "sticky"; }
#else
inline const char* deviceKindUpper() { return gDeviceIsX3 ? "X3" : "X4"; }
inline const char* deviceKindLower() { return gDeviceIsX3 ? "x3" : "x4"; }
#endif
