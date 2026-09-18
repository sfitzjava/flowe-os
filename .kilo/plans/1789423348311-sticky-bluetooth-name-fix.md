# Report "Sticky" as the device kind on the Sticky build

## Problem
On the reTerminal Sticky (`-e sticky`), every device-kind label falls through the X3/X4 ternary `gDeviceIsX3 ? … : …` and reports X4, because `gDeviceIsX3=false` is the shared "not X3" default (main.cpp:296). User-visible impact confirmed at three sites:

| Site | Current output on Sticky | Consumer |
|---|---|---|
| `src/scenes/SettingsScene.cpp:400` (footer `xphone-os %s (%s)`) | `(x4)` | on-glass Settings screen — the reported bug |
| `src/ble/CompanionBleService.cpp:2599` (`sendDeviceInfo` JSON `"device"`) | `"x4"` | iOS companion app (`device.info` notification) |
| `src/net/FileTransferServer.cpp:682` (status JSON `doc["device"]`) | `"X4"` | web/browser transfer client |

Already correct, no change: serial boot log (main.cpp:308-312 prints `device = Sticky (compile-time)` under `#if FREEINK_DEVICE_STICKY`), stall report (main.cpp:355 uses `BoardConfig::isSticky() ? "Sticky" : "X4"`), and the `deviceName() + 7` slice (CompanionBleService.cpp:2313) — "xphone Sticky" keeps the 7-char `"xphone "` prefix, so it yields "Sticky".

## Decision (user-approved)
Fix all three sites. The Sticky binary only ever runs on Sticky hardware, so x3/x4 outputs are untouched. Protocol caveat accepted: the iOS app receives `"device":"sticky"` where it previously got `"x4"`; it discovers/matches peers by SERVICE_UUID (CompanionProtocol.h:20-22), not by this label.

## Design
One shared accessor pair in `src/DeviceKind.h` (the existing header that declares `extern bool gDeviceIsX3`), compile-time-gated exactly like the `deviceName()` Sticky fix and the main.cpp boot gate:

```cpp
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
```

Case convention per site (matches each site's existing casing): Settings footer and HTTP status use `deviceKindUpper()`; BLE `device.info` uses `deviceKindLower()`. "Sticky" is capitalized as a brand name even where x3/x4 render lowercase.

## Tasks
1. `src/DeviceKind.h` — append the helper block above after the `extern bool gDeviceIsX3;` declaration.
2. `src/scenes/SettingsScene.cpp:400` — replace `gDeviceIsX3 ? "x3" : "x4"` with `deviceKindUpper()`; ensure `#include "../DeviceKind.h"` is present (add if the file currently relies on a transitive include or a local extern). Footer buffer is `char[64]`; `"xphone-os <ver> (Sticky)"` fits with margin.
3. `src/ble/CompanionBleService.cpp:2599` — replace `gDeviceIsX3 ? "x3" : "x4"` with `deviceKindLower()`. No include change needed: the TU already gets DeviceKind.h via `CompanionProtocol.h:3`. `char json[300]` absorbs the +4 bytes ("sticky" vs "x4").
4. `src/net/FileTransferServer.cpp:680-683` — delete the local `extern bool gDeviceIsX3;` block and set `doc["device"] = deviceKindUpper();`; add `#include "../DeviceKind.h"` near the existing includes (check whether it already includes it transitively; make it direct regardless since the extern goes away).
5. Sweep: `rg 'gDeviceIsX3 \? "[xX]3"' xphone-os/src` must return nothing outside `DeviceKind.h` (main.cpp:311 and 355 use the X3/X4 detect phrasing but are already Sticky-gated/handled — leave them).
6. Verify builds: `cd xphone-os && pio run -e sticky && pio run -e x3 && pio run -e x4` — all SUCCESS, no new warnings.

## Validation on hardware (when a Sticky is available)
- Settings footer reads `xphone-os <ver> (Sticky)`.
- iOS companion `device.info` payload shows `"device":"sticky"`.
- HTTP status endpoint JSON shows `"device":"Sticky"`.
- X3/X4 units still report x3/x4 and X3/X4 exactly as before (helper's `#else` branch is byte-identical logic).

## Risks
- iOS app behavior keyed on `device == "x4"` (if any exists in BluetoothManager/DeviceInfo handling) would take its default branch for `"sticky"`. Accepted by the user; if the app misbehaves, revert site 3 alone (one line).
- None for x3/x4 builds: the `#if` gate is compile-time and `FREEINK_DEVICE_STICKY` defaults to 0 (BoardConfig.h:52-53) in C3 envs.

## Out of scope
- Renaming the BLE advertised name (already "xphone Sticky" via CompanionProtocol.h:27-32).
- main.cpp boot-log/stall-report labels (already Sticky-aware).

Supersedes: prior plans in this file (Bluetooth device name — implemented; SdFat `DISABLE_FS_H_WARNING` — implemented and verified with `pio run -e sticky`).
