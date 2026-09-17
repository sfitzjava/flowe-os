# Porting xphone-os (Xteink X4 / ESP32-C3) to the reTerminal Sticky (ESP32-S3R8)

Status: implementation-ready plan
Date: 2026-09-14

> Note: the request asked for this plan at `xphone-os/Sticky.md`; the current
> edit permissions only allow writing under `.kilo/plans/`, so the plan lives
> here. Move it to `xphone-os/Sticky.md` when implementing if you want it in
> the project root.

## 0. What this is

xphone-os currently ships one universal ESP32-C3 binary that drives the Xteink
X3 (UC8253, 792x528) and X4 (SSD1677, 800x480), picked at boot by an I2C
fingerprint (`freeink::selectXteinkDevice()`). This plan adds a **third
device**, the Seeed **reTerminal Sticky** ("Sticky"), as a **separate build
env** (`sticky`) — not into the universal image, because the Sticky is an
**ESP32-S3** (Xtensa, dual-core) and the X3/X4 are **ESP32-C3** (RISC-V,
single-core). The FreeInk SDK already forbids mixing MCU families in one
binary (`BoardConfig.h` line 70: `#error "...all selected devices must share
one MCU family"`).

The Sticky is, electrically, very close to the X4: same SSD1677 panel
controller, same 800x480 resolution, same BQ27220 fuel gauge. The SDK already
carries a complete, triple-sourced `STICKY` board profile
(`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:841-890`). Most
of the port is therefore **build system + board-support glue + a new input
model**, not driver work.

### Hardware deltas that drive every code change

| | Xteink X4 (today) | reTerminal Sticky | Consequence |
|---|---|---|---|
| MCU | ESP32-C3 (RISC-V, 1 core, 160 MHz) | ESP32-S3R8 (Xtensa LX7, 2 cores, 240 MHz, 8 MB octal PSRAM) | new board/toolchain; all C3-isms audited (§3) |
| Flash | 16 MB | 32 MB QSPI | new partition table (§3.4) |
| Panel | SSD1677 800x480 | SSD1677 800x480 | **no display driver work** — SDK reuses it (BoardConfig.h:79, 826-829) |
| Front buttons | 4 (Back/Confirm/Left/Right), ADC ladder on GPIO1 | **none** | replaced by **capacitive touch taps** on the existing soft-key bar (§5) |
| Side buttons | 2 (Up/Down), ADC ladder on GPIO2 | 2 (Up=GPIO5, Down=GPIO6), digital | direct map to `Btn::Up`/`Btn::Down` = page up/down (§5.2) |
| Power button | GPIO3, active-low, deep-sleep wake | GPIO4, active-low, doubles as **OK/Confirm**, deep-sleep wake | shared-pin state machine (§5.3) |
| Power hold | GPIO13 battery-latch MOSFET (open = off) | **GPIO45 hold HIGH + GPIO46 pulse** to stay on | new board-init, must run in the first moments of `app_main()` (§4) |
| Touch | none | GT911 capacitive, own I2C bus (SDA3/SCL2, INT21, RST41, addr 0x5D/0x14, enable GPIO42) | new input path (§5.4) |
| Battery | ADC divider on GPIO0 | BQ27220 on **Wire1** (SDA1/SCL0, `i2cBus=1`) | `BatteryGauge.cpp` must honor `i2cBus` (§6) |
| Charger status | none | BQ25616 CHARGE_STATE on GPIO40 | optional polish (§6) |
| SD card | SPI, CS=12, shared EPD bus | SPI, CS=8, MISO=12, **power-enable GPIO10**, shared EPD bus | SDK `SDCardManager` already handles the rail (§3.5) |

Everything in the "Sticky" column above is already encoded in the SDK's
`STICKY` profile; the xphone-os side only has to stop assuming C3/Xteink in a
dozen places.

---

## 1. Build env: `sticky`

Add one env to `xphone-os/platformio.ini`. It does **not** extend the C3
`[base]` — the board, flash size, and several flags differ — it extends a new
`[base-s3]` so the C3 `[base]` is untouched (zero regression risk to shipping
X3/X4 builds).

```ini
; --- reTerminal Sticky — ESP32-S3R8, SSD1677 800x480 + GT911 touch -----------
; Separate S3 base: different MCU family from the C3 X3/X4 image (BoardConfig.h
; forbids mixing families in one binary). Single-device build — DEFAULT_DEVICE
; becomes STICKY, so no runtime detection runs (see main.cpp §2).
[base-s3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.37/platform-espressif32.zip
board = esp32-s3-devkitc-1          ; S3R8: 8 MB octal PSRAM, 32 MB flash variant below
framework = arduino
monitor_speed = 115200
upload_speed = 921600

board_build.arduino.memory_type = qio_opi   ; quad flash + octal PSRAM (S3R8)
board_upload.flash_size = 32MB
board_upload.maximum_size = 33554432
board_upload.offset_address = 0x10000
board_build.flash_mode = qio
board_build.flash_size = 32MB
board_build.partitions = partitions-sticky.csv   ; §3.4

extra_scripts =
  pre:tools/gen_version.py
  pre:tools/patch_ble_service_friend.py   ; still needed — same BLEService orphan on S3 (verify, §3.3)
  post:tools/lto_link.py

build_flags =
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1        ; S3 native USB OTG CDC (replaces C3 USB Serial/JTAG)
  -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DPNG_MAX_BUFFERED_PIXELS=16416
  -DARDUINO_NETWORK_EVENT_TASK_STACK_SIZE=2048
  -DUSE_UTF8_LONG_NAMES=1
  -DFREEINK_DEVICE_STICKY=1          ; the only device in this build
  -DXP_BATT_DESIGN_MAH=750           ; Sticky pack is 750 mAh (Hardware Overview), not the X3's 650
  -std=gnu++2a
  -fno-exceptions
  -flto

build_unflags =
  -std=gnu++11
  -fexceptions

lib_deps =
  BoardConfig=symlink://../freeink-sdk/libs/hardware/BoardConfig
  EInkDisplay=symlink://../freeink-sdk/libs/display/FreeInkDisplay
  SDCardManager=symlink://../freeink-sdk/libs/hardware/SDCardManager
  InputManager=symlink://../freeink-sdk/libs/hardware/InputManager
  BatteryMonitor=symlink://../freeink-sdk/libs/hardware/BatteryMonitor
  bblanchon/ArduinoJson @ 7.4.2
  ; NOTE: XteinkDetect is deliberately NOT linked — it is Xteink-only (§2).

[env:sticky]
extends = base-s3
```

Flags that come **free** from `FREEINK_DEVICE_STICKY=1` (BoardConfig.h):
`FREEINK_MCU_S3`, `FREEINK_DRIVER_SSD1677` (line 79), `FREEINK_CAP_TOUCH`
(line 129-132), `FREEINK_BATTERY_I2C_GAUGE` (line 166), `FREEINK_CAP_MIC/RTC/
TEMP_HUMIDITY/IMU/BUZZER`, and `DEFAULT_DEVICE = STICKY` (line 923-924).
Do **not** pass `FREEINK_DEVICE_X3/X4` — they would force the C3 family check
to fail.

The C3-only experiment envs (`x3sleep`, `x3lean`, `x3ls`, `x3mem`) stay C3-only;
nothing about them is ported in this milestone.

---

## 2. Boot: bypass Xteink detection, select the Sticky profile

`main.cpp boot()` stage 2 currently calls `freeink::selectXteinkDevice()` — an
I2C probe for X3-only parts on SDA20/SCL0 (`XteinkDetect.h`). On the Sticky
those pins are *the gauge bus* (SDA1/SCL0 is the gauge; SDA20 doesn't exist),
so the probe is meaningless. Gate it:

```cpp
// main.cpp, in boot() where selectXteinkDevice() runs today.
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  // Single-device S3 build: no Xteink fingerprint. BoardConfig::ACTIVE is
  // already STICKY via DEFAULT_DEVICE; selectDevice() is called explicitly so
  // the intent is greppable and matches the X3/X4 path's structure.
  BoardConfig::selectDevice(BoardConfig::Board::Sticky);
  gDeviceIsX3 = false;   // Sticky is "not X3": X3-only paths (IMU sleep, UC8253
                         // partials, gauge-on-Wire) all key off this flag.
#else
  gDeviceIsX3 = freeink::selectXteinkDevice();
  if (gDeviceIsX3) Sleep::imuSleepAtBoot();
#endif
```

`gDeviceIsX3 == false` is exactly right for the Sticky everywhere it is read
(`Scene.cpp:369/377` refresh-tier choice, `Sleep.cpp:180` nap poster tier,
`main.cpp` charging/USB-window checks) — those branches are the X4 behavior,
and the Sticky's SSD1677 wants the X4 behavior. No new `gDeviceIsSticky` flag
is introduced; where Sticky truly differs (power latch, shared OK/power pin)
the code keys off `BoardConfig::isSticky()` / `FREEINK_DEVICE_STICKY` directly,
which is the SDK's idiom.

Also drop the now-dead include path: `#include <XteinkDetect.h>` and the
`XteinkDetect` lib_dep are Xteink-only (guarded out for Sticky).

---

## 3. MCU migration: ESP32-C3 → ESP32-S3 audit

### 3.1 USB serial
C3 uses the USB **Serial/JTAG** peripheral; S3 uses native **USB OTG** with its
own CDC. The build flags `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1`
are the correct pair on both — no code change, but **verify on hardware** that
the Sticky enumerates as a CDC port and that `Serial` logs appear. The SDK
already hints Sticky logging is happier on the ROM console
(`FREEINK_LOG_TRANSPORT_ROM_PRINTF`, BoardConfig.h:234-236); if USB CDC proves
flaky during bring-up, the fallback is UART on GPIO43/44. Decide after first
boot; do not block the port on it.

### 3.2 BLE stack (NimBLE)
xphone-os's BLE companion + ANCS client compile against the Arduino core's
built-in BLE library, which on the C3 prebuilt libs is backed by **NimBLE**
(`CONFIG_BT_NIMBLE_ENABLED=y` — platformio.ini comment, lines 82-88). The
pioarduino S3 prebuilt libs in the same `55.03.37` package also default to
NimBLE, but this must be **verified, not assumed**:

- Check `framework-arduinoespressif32-libs/esp32s3/sdkconfig` for
  `CONFIG_BT_NIMBLE_ENABLED=y` / `CONFIG_NIMBLE_ENABLED=y`.
- If the S3 libs instead ship Bluedroid, the companion service
  (`src/ble/CompanionBleService.cpp`) and ANCS client
  (`src/ble/CompanionAncsClient.cpp`) need re-validation; the code is written
  against the Arduino BLE wrapper API, which is stack-agnostic, so the expected
  outcome is "works, but re-run the pairing + ANCS backfill soak" (README M2
  pairing flow).
- `patch_ble_service_friend.py` patches the pinned framework's `BLEService.h`;
  confirm the S3 framework package has the same file at the same relative path
  (it should — same core, different `esp32s3` lib dir).

Unaligned-access note: `CompanionAncsClient.cpp:167` documents that the C3
faults on unaligned wide loads; the Xtensa LX7 has the same restriction, so the
existing packed/aligned handling carries over unchanged.

### 3.3 CPU frequency policy
`CpuBoost.h` parks at `XP_CPU_MHZ=80` and boosts to 160 for reader work. The S3
supports 80/160/240 and `setCpuFrequencyMhz()` works identically. Keep 80/160
for the first boot (known-good BLE floor), then optionally raise the boost to
240 as a follow-up A/B (`-DXP_CPU_MHZ` and the boost constant are the only two
knobs). Out of scope for the bring-up milestone.

### 3.4 Flash / partitions (16 MB → 32 MB)
`partitions.csv` is sized for 16 MB (`coredump` ends at 0x1000000). Ship a
`partitions-sticky.csv` that keeps the **same layout** (OTA0/OTA1 + spiffs +
coredump) so `SdUpdate.cpp`'s raw `esp_partition` OTA scheme works unchanged,
and simply uses the larger space:

```csv
# Name,   Type, SubType, Offset,   Size,      Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x640000,
app1,     app,  ota_1,   0x650000, 0x640000,
spiffs,   data, spiffs,  0xc90000, 0x360000,
coredump, data, coredump,0xFF0000, 0x10000,
```

Keeping app/spiffs sizes identical to the C3 table is deliberate: the SD
self-update validates the image against the *inactive* OTA slot and switches
`otadata` raw (`SdUpdate.cpp`); identical geometry means that logic is
byte-for-byte reused. Growing the app slots (e.g. 2x 8 MB) is a safe later
optimization, not part of bring-up.

### 3.5 SPI / SD bus sharing
The Sticky shares one SPI bus between the SSD1677 panel and the SD card, with
the SD behind its own CS (GPIO8) and a power rail (GPIO10). The SDK's
`SDCardManager` already asserts `sd.powerEnable` before mount
(`SDCardManager.cpp:44`) and `EpdBus` already asserts the panel's
`powerEnable` (GPIO47) before SPI bring-up (`EpdBus.cpp:12-18`), so **no
xphone-os code changes here** — but `main.cpp`'s one-time `SPI.begin(...)` call
must use the Sticky's pins. It already reads them from
`BoardConfig::ACTIVE.display.{sclk,mosi,cs}` and `BoardConfig::ACTIVE.sd.miso`
(main.cpp:295-296), which resolve to the STICKY profile automatically once §2
lands. Hardware-validate the shared-bus CS arbitration (the SDK flags this as
inferred, BoardConfig.h:852-855): if the panel and SD interfere, drop
`displaySpiHz` to 10 MHz (the value Seeed's own demo uses) via the profile.

### 3.6 PSRAM
8 MB octal PSRAM is enabled by `board_build.arduino.memory_type = qio_opi`. The
firmware's 48 KB static framebuffer stays in internal SRAM
(`FREEINK_FB_PSRAM` is only default-on for the classic-ESP32 M5Paper,
BoardConfig.h:214-216) — the S3 has 512 KB SRAM and does not need PSRAM for the
framebuffer. No code change; just confirm the build links with PSRAM found
(`psramFound()` in the boot report would be a reasonable addition to About).

### 3.7 Things that do **not** change
- Single-framebuffer `EINK_DISPLAY_SINGLE_BUFFER_MODE`, expat flags, PNGdec
  buffer, SdFat UTF-8 names, ArduinoJson pin — all MCU-agnostic.
- The bespoke `Gfx` blitter and EpdFontData fonts — pure C++, no C3-isms.
- `SdUpdate.cpp` OTA mechanics — raw partition API, MCU-agnostic.
- The scene/store/BLE architecture — all portable C++.

---

## 4. Board support: the power latch (the one truly new driver)

The Sticky powers off the instant the power button is released unless firmware
latches the rail. Per the Sticky hardware references (Seeed ESP-IDF device
guide; the `sticky-2048` reference firmware's board init), the sequence is:
**drive GPIO45 HIGH and pulse GPIO46**, inside the first moments of boot.

This is board-support, not SDK — the SDK's `PowerManager` lib has no Sticky
latch today (the GPIO13 latch in `Sleep.cpp:548-562` is the *opposite*
mechanism: the X3/X4 latch is opened to cut power on battery). Add a small
board-init and call it before everything else in `boot()`:

```cpp
// src/BoardSticky.h — Sticky board bring-up that has no SDK home.
#pragma once
namespace BoardSticky {
// Latch the main power rail so the device stays on after the power button is
// released. MUST run in the first moments of boot — before serial, before the
// display. Idempotent. No-op on non-Sticky builds.
void powerHold();
}
```

```cpp
// src/BoardSticky.cpp
#include "BoardSticky.h"
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
#include <Arduino.h>
#include <driver/gpio.h>

void BoardSticky::powerHold() {
  // GPIO45/46 are both ESP32-S3 strapping pins — they are only sampled at
  // reset, so by the time app code runs they are free to drive. Do not attach
  // anything that pulls them at boot (see ESP32-S3 strapping docs).
  gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_45, 1);            // hold the rail
  gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_46, 1);
  gpio_set_level(GPIO_NUM_46, 0);            // pulse
  gpio_set_level(GPIO_NUM_46, 1);
}
#else
void BoardSticky::powerHold() {}
#endif
```

Call it as the literal first statement of `boot()` (before `delay(250)`):

```cpp
static void boot() {
  BoardSticky::powerHold();   // Sticky: latch the rail before anything can print
  const unsigned long tBoot = millis();
  ...
}
```

**Consequence for `Sleep::sleepNow()`:** the X3/X4 path opens the GPIO13 latch
to power the MCU off on battery. The Sticky must **not** do that — it has no
GPIO13 latch, and its power stays held by GPIO45. Gate the whole latch block,
and keep deep sleep as the "off" state (GPIO45 stays driven; the rail latch
holds through deep sleep because the S3 keeps RTC-domain GPIO state when
`gpio_deep_sleep_hold_en()` is set — verify on hardware that GPIO45 needs a
hold or is in the always-on domain; if it droops, add
`gpio_hold_en(GPIO_NUM_45)` + `gpio_deep_sleep_hold_en()`):

```cpp
// Sleep.cpp sleepNow(), replacing the unconditional GPIO13 block.
if (!BoardConfig::isSticky()) {
  // X3/X4 only: GPIO13 drives the battery-latch MOSFET (cut power on battery).
  constexpr gpio_num_t kBatteryLatchPin = GPIO_NUM_13;
  gpio_set_direction(kBatteryLatchPin, GPIO_MODE_OUTPUT);
  gpio_set_level(kBatteryLatchPin, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(kBatteryLatchPin);
} else {
  // Sticky: keep the rail latched through deep sleep; wake is GPIO4 (below).
  gpio_hold_en(GPIO_NUM_45);
  gpio_deep_sleep_hold_en();
}
```

### Deep-sleep wake source
- C3 has no ext0/ext1, so xphone-os uses `esp_deep_sleep_enable_gpio_wakeup()`
  (`Sleep.cpp:571`). The S3 **does** have ext0/ext1, and GPIO4 is an RTC GPIO,
  so either API works. Keep the existing `esp_deep_sleep_enable_gpio_wakeup(1ULL
  << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW)` — the pin comes
  from the STICKY profile (`input.power = 4`) automatically, and the C3-style
  GPIO wakeup is supported on S3. No change needed; confirm wake on hardware.
  (ext1 is the fallback if the GPIO wakeup proves unreliable.)

---

## 5. Input: touch replaces the 4 front buttons; 2 physical buttons = page up/down

This is the heart of the request. Today `Input.h` wraps the SDK `InputManager`
with a fixed logical map (`kMap`: Up/Down/Left/Right/Confirm/Back → SDK
BTN_UP/DOWN/LEFT/RIGHT/CONFIRM/BACK) fed by the Xteink ADC ladders. On the
Sticky the SDK runs `InputStyle::DigitalButtons`:

- `getDigitalState()` (InputManager.cpp:171-189) reads the profile's pins:
  **up=GPIO5 → BTN_UP, down=GPIO6 → BTN_DOWN, confirm=GPIO4 → BTN_CONFIRM,
  power=GPIO4 → BTN_POWER**. Back/Left/Right are `PIN_UNASSIGNED` (-1) and can
  never come from a physical button — **they must come from touch.**
- The `kMap` in `Input.h` uses board-agnostic `InputManager::BTN_*` indices, so
  Up/Down/Confirm/Power flow through the existing tap/long-press state machine
  **unchanged**. No `Input.h` mapping change is needed for the physical
  buttons.

### 5.1 The mapping (decided)
- **Physical UP (GPIO5) → `Btn::Up`** and **DOWN (GPIO6) → `Btn::Down`**.
  Every list/reader scene already consumes these as page-up/page-down or
  cursor-up/down (`ReaderScene.cpp:1018-1019`, `NotificationsScene.cpp:165-170`,
  `PrioritiesScene.cpp:197-201`, etc.), so this requirement is satisfied by the
  BoardConfig profile alone. In the reader they are the page-turn keys.
- **Physical OK (GPIO4) → `Btn::Confirm` + power/wake** — one pin, two roles,
  disambiguated by hold time (§5.3).
- **Touch tap on a soft-key tab → `Btn::Back` / `Btn::Confirm` / `Btn::Left` /
  `Btn::Right`** (§5.4) — the on-screen tabs are the replacement front buttons.
- **Touch swipe left/right → `Btn::Left` / `Btn::Right`** for list and reader
  navigation (§5.5).

### 5.2 Why "page up/down" is already right
The SDK profile comment is explicit ("PWR/UP/DOWN...; confirm/back come from
touch", BoardConfig.h:844) and xphone-os's scenes treat `Btn::Up/Down` as the
primary scroll/page keys everywhere. So "link the 2 buttons to page up/page
down" = the default map, already correct.

### 5.3 The shared OK/power pin (GPIO4) — a state-machine change
Today power and Confirm are different pins. On the Sticky they are one pin, and
the two existing gesture sets collide:

- `Input.h`: Confirm **tap** fires on release before `kLongPressMs` (550 ms);
  Confirm **long-press** fires at 550 ms while held.
- `main.cpp checkPowerButton()`: power release < 2500 ms = **nap**, ≥ 2500 ms =
  **off**, ≥ 8000 ms = **restart**.

If left as-is, a 600 ms–2500 ms hold would fire *both* a Confirm long-press and
a nap. Resolve by making the power gestures strictly longer than any Confirm
gesture, on Sticky only:

| Hold time on GPIO4 (Sticky) | Result |
|---|---|
| release < 550 ms | **Confirm tap** (normal Confirm) |
| 550 ms – 1500 ms | **Confirm long-press** (existing scene behavior) |
| release ≥ 1500 ms and < 5000 ms | **nap / wake** |
| held ≥ 5000 ms | **off (deep sleep)** |
| held ≥ 10000 ms | **restart** |

Implementation:
- In `Input.h`, when `BoardConfig::isSticky()`, *exclude* GPIO4-as-Confirm from
  the 550 ms long-press ladder for the power path only — the cleanest split is
  to let the existing Confirm tap/long logic run exactly as today (it produces
  the < 550 ms / ≥ 550 ms Confirm events), and change **only** the power
  thresholds in `checkPowerButton()` for Sticky: `kNapHoldMs = 1500`,
  `kOffHoldMs = 5000`, `kRestartHoldMs = 10000`. Because a nap/off/restart hold
  is always ≥ 1500 ms, it is always past the Confirm long-press, so the rule
  becomes: **a hold that reaches the nap threshold must suppress the Confirm
  long-press.** Do that by having `checkPowerButton()` treat any release with
  `held >= kNapHoldMs` as power-only and adding one line to `Input.h`'s
  tap/long machine: on Sticky, if the button is `Confirm` and the hold crossed
  `kNapHoldMs`, swallow the pending Confirm events (the way a consumed
  long-press already swallows its release-tap today).
- The wake press from deep sleep is still GPIO4 — unchanged hardware role.
- Document the new thresholds next to `kLongPressMs` and in the Settings >
  About help text.

### 5.4 Touch: tap the soft-key tabs = the 4 replaced buttons
The M2.1 soft-key bar already draws 4 tabs at the bottom of every scene,
aligned to where the X4's physical buttons sit (`Scene.cpp` `drawSoftKeyBar`,
`softKeyTabGeom()`). On the Sticky these tabs become the buttons. The SDK
already does the hard part — GT911 init with power-enable + reset dance
(`InputManager.cpp:603-664`), polled reads, tap/swipe detection with normalized
panel-frame coordinates (`wasTouchTap()`, `wasSwipe()`).

**Coordinate frames.** The GT911 delivers points in the panel's native
landscape frame (800x480 after the profile's `swapXY`/`flipX`/`flipY`
correction). `Gfx` draws every scene in **logical portrait** (480x800) with a
90° CW rotation in `drawPixel()`: `phyX = y; phyY = _w - 1 - x`
(`Gfx.cpp:48-49`). Tap routing needs the inverse — native → logical:

```cpp
// Inverse of drawPixel()'s portrait transform (Gfx.cpp:48-49):
//   phyX = y;            phyY = logicalW - 1 - x
// so: x = logicalW - 1 - phyY;   y = phyX
void Input::nativeToLogical(int& x, int& y, int logicalW) const {
  const int nx = logicalW - 1 - y;
  const int ny = x;
  x = nx; y = ny;
}
```

(This mirrors x4-os's `GfxRenderer::tapToLogical`, referenced in
BoardConfig.h:376 — same idea, re-derived for xphone-os's own transform. When
the Gfx orientation is `Landscape` (reader only), the transform is the
identity.)

**Routing.** Add to the `Input` class:

```cpp
// In Input.h public API (Sticky/touch builds only; inert stubs otherwise):
// A tap that landed on soft-key slot `slot` (0..3), as a synthetic button tap.
// Drained through update() exactly like a physical press, so scenes need no
// changes.
bool touchTapToSoftKey(int logicalX, int logicalY);  // returns slot or -1
```

and in the sampling task (`Input::sampleOnce()`, after `_mgr.update()`):

```cpp
#if FREEINK_CAP_TOUCH
  float nx, ny;
  if (_mgr.wasTouchTap(nx, ny)) {
    // Panel-native (0..1) -> native px -> logical portrait px.
    const int natX = (int)(nx * BoardConfig::ACTIVE.displayWidth);
    const int natY = (int)(ny * BoardConfig::ACTIVE.displayHeight);
    int lx = natX, ly = natY;
    if (G_GFX && G_GFX->orientation() == Gfx::Orient::Portrait)
      nativeToLogical(lx, ly, /*logicalW=*/BoardConfig::ACTIVE.displayHeight);
    // Hit-test the soft-key bar: the bottom SOFTKEY_BAR_H strip, 4 slots.
    const int slot = softKeySlotAt(lx, ly);   // Scene.cpp's softKeyTabGeom, exported
    if (slot >= 0) {
      // Soft-key slot order is Back=0, Confirm=1, Left=2, Right=3 — the same
      // fixed map as kMap. Inject as a tap on the matching logical button.
      static constexpr Btn kSlotBtn[4] = {Btn::Back, Btn::Confirm, Btn::Left, Btn::Right};
      injectTap(kSlotBtn[slot]);   // existing latch path — zero new plumbing
    }
    _lastActivityMs = now;  // touch is activity (idle/nap timers)
  }
#endif
```

Key points:
- Reuse `injectTap()` (Input.h:156) so a touch tap enters the exact same
  latch→`update()`→`wasPressed()` path as a physical press. **Every scene works
  unchanged** — LauncherScene's `Btn::Back` opens Settings, `Btn::Confirm`
  opens the selected app, etc.
- `softKeySlotAt()` is the hit-test twin of `softKeyTabGeom()` (Scene.cpp:107):
  export a non-drawing helper from Scene.cpp that returns the slot whose tab
  rect contains `(lx,ly)`, plus the existing touch slop. The bar's
  `kBarMarginPct`/rocker math is already centralized there, so the hit rects
  always match the drawn tabs.
- Enlarge the touch targets using the profile's `uiScale` (Sticky ships 1.2,
  BoardConfig.h:890): either grow `SOFTKEY_BAR_H` on Sticky or inflate the hit
  rect by ~20% beyond the drawn tab. Finger targets at 235 PPI need ~44+ px.

### 5.5 Touch: swipes for navigation
Per the decision (tabs **and** gestures), add swipe → directional button in the
same touch block:

```cpp
  float sx0, sy0, sx1, sy1;
  if (_mgr.wasSwipe(sx0, sy0, sx1, sy1)) {
    // Work in the panel-native frame; the SDK already reports dominant-axis
    // distance (TOUCH_SWIPE_MIN_PX). Map native X to logical -x/+x.
    const float dxn = sx1 - sx0;
    if (dxn <= -kSwipeMin)      injectTap(Btn::Left);   // native left  = logical left
    else if (dxn >= kSwipeMin)  injectTap(Btn::Right);  // native right = logical right
    _lastActivityMs = now;
  }
```

The scenes that pair Left/Right with Up/Down (Notifications, Priorities, Today,
Settings, Wifi, Reader) then scroll/page on a swipe with **no per-scene
changes** — the same reason the tab-tap route is free. The reader's
`backKey()/fwdKey()` already accept Left/Right, so swipe turns pages.
Vertical swipes are deliberately unmapped in v1 (Up/Down have physical
buttons); add later if a scene wants them.

### 5.6 Touch bring-up risk
The SDK's STICKY touch config is marked confirmed by bring-up taps
(BoardConfig.h:868-871) but the *panel mount orientation* is still "pending
validation" (`NO_FLIP`, line 876). If the first hardware boot shows the image
upside-down or the touch mirrored, the fix is a one-line profile change
(`orientation` / `flipX`/`flipY`), not app code — call this out in the
validation checklist (§8) rather than pre-solving it.

---

## 6. Battery gauge on Wire1

The SDK's `BatteryMonitor` already honors `batteryGauge.i2cBus` and uses
`Wire1` for the Sticky (`BatteryMonitor.cpp:28-41`), so the launcher/About
battery reads work once the profile is active. The xphone-os-only
`BatteryGauge.cpp` (design-capacity reprogram + average-current read for the
charging bolt) talks to the gauge over **`Wire`** directly. On the Sticky the
gauge is on **Wire1** (SDA1/SCL0), so add the same bus-selection idiom:

```cpp
// BatteryGauge.cpp — mirror BatteryMonitor's gaugeWire() (SDK, lines 28-33).
static TwoWire& gaugeWire() {
#if SOC_I2C_NUM > 1
  if (BoardConfig::ACTIVE.batteryGauge.i2cBus == 1) return Wire1;
#endif
  return Wire;
}
```

and route every `Wire.` call in that file through `gaugeWire()`. Also:
- `XP_BATT_DESIGN_MAH` becomes 750 via the build flag (§1).
- Charging indicator: the X4 ADC path has no charge detection; the Sticky has
  BQ25616 `CHARGE_STATE` on **GPIO40** (`batteryChargeStatus` in the profile).
  `LauncherScene.cpp:319` and `main.cpp:1471` currently gate the bolt on
  `gDeviceIsX3 && readAvgCurrentMa(...)`. Extend the charging check to also
  honor a digital charge-status pin when the profile provides one
  (`BoardConfig::ACTIVE.batteryChargeStatus >= 0`), so the bolt works on the
  Sticky without pretending it is an X3.

---

## 7. Everything else: explicit non-goals and leftovers

- **Do not** port the light-sleep experiment envs (`x3sleep/x3lean/x3ls/x3mem`)
  or their custom C3 library packages. The Sticky env uses the stock pioarduino
  S3 libs.
- **Do not** wire the PDM mic, IMU, RTC, SHT40, or buzzer into features. The
  SDK compiles their libs in (`FREEINK_CAP_*`), but no xphone-os scene uses
  them. A future milestone can use the buzzer for key feedback (the SDK's
  `Buzzer` lib drives GPIO48) — noted, not planned.
- **Wi-Fi file transfer / reader / BLE companion / ANCS** are MCU-agnostic and
  need only re-validation on S3 (§3.2), not redesign.
- The soft-key **bar stays visible** on the Sticky — it is now the primary
  button affordance, not just a hint.
- `Input.h`'s bench injection (`injectTap/injectHold/injectPower`) is reused by
  the touch route, so the dev console keeps working.

---

## 8. Ordered task list

1. **Build skeleton.** Add `[base-s3]` + `[env:sticky]` and
   `partitions-sticky.csv` (§1, §3.4). `pio run -e sticky` must compile and
   link before any hardware work. Fix any S3-only compile errors (expected to
   be few; the code is largely portable C++).
2. **Power latch.** Add `BoardSticky.{h,cpp}`, call `powerHold()` first in
   `boot()` (§4). Without this the device dies when the button is released.
3. **Profile selection.** Gate out `selectXteinkDevice()` for Sticky, call
   `selectDevice(Board::Sticky)` (§2). Remove the `XteinkDetect` lib_dep from
   the Sticky env.
4. **First boot on hardware.** Verify: power stays latched, serial logs,
   display paints the boot splash + launcher (SSD1677 path shared with X4),
   SD mounts (shared-bus CS check, §3.5), battery reads on Wire1 (§6).
5. **Physical buttons.** Confirm UP/DOWN scroll lists and turn reader pages,
   and OK taps Confirm — all through the existing `kMap` (§5.1-5.2).
6. **Deep sleep.** Gate the GPIO13 latch, add the GPIO45 hold, verify GPIO4
   wake + last-scene restore (§4).
7. **Touch bring-up.** Enable/verify GT911 (SDK does init); add a temporary
   serial dump of `wasTouchTap` coordinates to confirm the native→logical
   transform and the profile's flip flags on real glass (§5.6).
8. **Tab taps.** Implement `nativeToLogical`, `softKeySlotAt` hit-test, and the
   `injectTap` route in `Input::sampleOnce()` (§5.4). Verify each of the 4
   tabs drives its button in Launcher/Notifications/Reader.
9. **Swipes.** Add swipe→Left/Right (§5.5). Verify list scroll + reader page
   turn.
10. **Shared-pin power gestures.** Implement the Sticky power thresholds and
    Confirm-suppression (§5.3). Verify tap=Confirm, 1.5 s=nap, 5 s=off,
    10 s=restart, and that a nap-hold does not also fire Confirm-long.
11. **BLE/ANCS re-validation.** Verify NimBLE on the S3 libs, run the pairing
    flow + ANCS backfill + a priorities/today sync soak (§3.2).
12. **Charging bolt + About.** Wire GPIO40 charge status, show PSRAM in About
    (§6, §3.6).
13. **Docs.** Update README env table (`sticky`, SSD1677 800x480) and the
    Controls table (touch tabs + UP/DOWN + OK gestures).

## 9. Validation checklist (hardware)

- [ ] Device stays powered after releasing the power button (latch).
- [ ] Boot splash → launcher on glass; no frozen panel.
- [ ] UP/DOWN scroll every list; reader pages turn.
- [ ] Each soft-key tab triggers its labeled action by touch.
- [ ] Swipe left/right navigates in Notifications and turns reader pages.
- [ ] OK tap = Confirm; 1.5 s = nap; 5 s = off; 10 s = restart; no
      Confirm-long bleed-through on holds.
- [ ] Deep sleep → GPIO4 wake → last scene restored (NVS restore path is
      MCU-agnostic, re-verify).
- [ ] SD `update.bin` self-update flashes the inactive OTA slot and reboots.
- [ ] iPhone pairing + ANCS backfill + card sync on S3.
- [ ] Battery % and charging bolt read correctly on Wire1/GPIO40.

## 10. Open questions / risks

1. **NimBLE on S3 libs** (§3.2) — the single biggest unknown; check sdkconfig
   first, before assuming BLE works.
2. **Panel mount orientation** (`NO_FLIP` pending) — one-line profile fix if
   the first boot is rotated/mirrored (§5.6).
3. **Shared SPI bus** panel/SD CS arbitration — inferred by the SDK; if flaky,
   drop display SPI to 10 MHz (§3.5).
4. **GPIO45 hold through deep sleep** — confirm the rail stays latched; add
   `gpio_deep_sleep_hold_en()` for GPIO45 if it droops (§4).
5. **USB CDC on S3** — if enumeration is unreliable, fall back to UART logging
   on GPIO43/44 during bring-up (§3.1).

## 11. References

- SDK Sticky profile: `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:825-890`
  (pins, touch, gauge bus, uiScale; family/driver/capability derivation at
  lines 52-73, 79, 129-132, 166, 923-924).
- SDK touch implementation (GT911): `freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:482-764`
  (`beginGt911`, `pollGt911`, `wasTouchTap`, `wasSwipe`).
- SDK digital-button path: `InputManager.cpp:167-189` (`getDigitalState`).
- SDK gauge bus selection: `freeink-sdk/libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp:26-41`.
- SDK SD power rail: `freeink-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp:44`;
  panel power rail: `freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.cpp:12-18`.
- xphone-os input wrapper: `xphone-os/src/Input.h` (`kMap` at 194-197, tap/long
  machine at 228-336, `injectTap` at 156).
- xphone-os soft-key bar geometry: `xphone-os/src/Scene.cpp:104-155`
  (`softKeyTabGeom`, `drawSoftKeyTab`).
- xphone-os logical→native transform: `xphone-os/src/Gfx.cpp:33-57`
  (`drawPixel` portrait rotation; the inverse is derived in §5.4).
- xphone-os power button + nap: `xphone-os/src/main.cpp:950-986`
  (`checkPowerButton`), deep sleep + GPIO13 latch: `xphone-os/src/Sleep.cpp:527-577`.
- xphone-os Xteink detection: `xphone-os/src/main.cpp:274-286`;
  `freeink-sdk/libs/hardware/XteinkDetect/include/XteinkDetect.h`.
- xphone-os X4 gauge assumption: `xphone-os/src/BatteryGauge.cpp` (uses `Wire`).
- reTerminal Sticky hardware (Seeed): Hardware Overview — ESP32-S3R8, 800x480
  4-gray e-paper, GT911 touch, UP/DOWN/AI(OK) buttons, 750 mAh, BQ27220,
  BQ25616, shared EPD/SD SPI. Seeed ESP-IDF device guide ("Pages and
  Peripherals"): bus map (I2C0=GT911 on GPIO2/3, I2C1=gauge/RTC/SHT40/IMU on
  GPIO0/1, SPI2 shared EPD+SD), power-rail management by module.
- Power latch + pin map corroboration: `sticky-2048` reference firmware board
  init (Power Hold/Lock GPIO45/46; OK/Power GPIO4; EP SPI 13-18; GT911
  2/3/21/41/42; gauge 0/1; buzzer 48) and the reTerminal Sticky community
  skill's hardware reference.
- ESP32-S3 strapping pins (GPIO0/3/45/46) and `gpio_deep_sleep_hold_en`:
  Espressif ESP-IDF GPIO/RTC-GPIO docs for ESP32-S3.
