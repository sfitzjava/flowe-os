# Bluetooth Device Name Fix for Sticky Build

## Problem
When building for the reTerminal Sticky (`pio run -e sticky`), the Bluetooth device advertises as "xphone X4" instead of "xphone Sticky".

## Root Cause
In `src/ble/CompanionProtocol.h:25`:

```cpp
inline const char* deviceName() { return ::gDeviceIsX3 ? "xphone X3" : "xphone X4"; }
```

This only differentiates between X3 and X4 using the runtime flag `gDeviceIsX3`. When building for the Sticky, `gDeviceIsX3` is set to `false` (since it's not X3), so the name defaults to "xphone X4".

## Fix
Update the `deviceName()` function to check the compile-time `FREEINK_DEVICE_STICKY` define (which is set in `[env:sticky]` in `platformio.ini` and defaults to `0` in `BoardConfig.h:53`):

```cpp
inline const char* deviceName() {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
    return "xphone Sticky";
#else
    return ::gDeviceIsX3 ? "xphone X3" : "xphone X4";
#endif
}
```

## Files to Change
- `xphone-os/src/ble/CompanionProtocol.h` - Line 25: Update `deviceName()` function

## Verification
1. Build all three envs to ensure no regressions:
   ```bash
   pio run -e sticky  # Should use "xphone Sticky"
   pio run -e x3      # Should use "xphone X3"
   pio run -e x4      # Should use "xphone X4"
   ```
2. Flash to Sticky hardware and verify the device name in iOS Bluetooth settings

## Notes
- `FREEINK_DEVICE_STICKY` is already included via `#include "../DeviceKind.h"` indirectly through the build system
- The `BoardConfig.h` header defines `FREEINK_DEVICE_STICKY` and defaults it to `0` if not overridden (line 52-53)
- This is a compile-time constant, so the preprocessor can optimize away the unused branches
