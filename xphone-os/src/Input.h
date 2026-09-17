#pragma once

// xphone-os M1 — logical button input.
//
// Thin wrapper over the FreeInk SDK InputManager (XteinkAdcLadder style on
// X3/X4: the 4 bottom-front buttons are a resistor ladder on ADC GPIO1, the
// pair on the TOP edge (held portrait) a second ladder on ADC GPIO2 —
// InputManager.cpp ADC_RANGES_1/2; the SDK calls that pair BTN_UP/BTN_DOWN).
// Fixed default mapping, no settings system: logical == the SDK's physical
// indices, same as x4-os CrossPointSettings defaults (FRONT_HW_BACK=0,
// CONFIRM=1, LEFT=2, RIGHT=3; the top pair Up=4 / Down=5 is always fixed —
// in x4-os default Portrait MappedInputManager does not remap any of them).
//
// M3 tap vs long-press: a per-button state machine on top of the SDK's
// debounced level state (isPressed). Timestamp latched on the press edge;
// holding past kLongPressMs fires wasLongPressed() ONCE (no repeat) while the
// button is still down; releasing before the threshold fires wasPressed() —
// i.e. a TAP is reported on RELEASE, not on the press edge, which is what
// lets a long-press consume the hold. The POWER button is deliberately
// excluded: it is owned by the hold-to-restart handler in main.cpp
// (powerPressed()/powerHeldMs()).
//
// M5 responsiveness (docs/x3-responsiveness-plan.md Phase 1): sampling runs
// on a DEDICATED FreeRTOS task every 5 ms (beginTask()), so presses register
// even while the loop task is busy (e-ink flushes used to eat any tap that
// started AND ended inside their 0.45–3.2 s window — SDK events are one-shot
// and were simply never seen). Events LATCH into pending bitmasks and are
// drained by update() on the main loop, so nothing is lost between ticks.
// Verified safe on the single-core C3: the display driver's busy-wait yields
// every 1 ms (EpdBus::waitBusy delay(1)) and holds no locks, and analogRead
// goes through the IDF oneshot driver's own mutex. The InputManager state is
// task-owned once the task starts — the main loop only reads the published
// snapshots below.
//
// Call update() once per loop() tick, then query wasPressed()/
// wasLongPressed() edges (same API as always).

#include <Arduino.h>
#include <InputManager.h>

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <BoardConfig.h>
#include "Gfx.h"

// Touch hit-test (Scene.cpp): which soft-key slot a LOGICAL point falls in.
// Forward-declared here (not #include "Scene.h") because Scene.h includes this
// header — the include would be circular.
int softKeySlotAt(Gfx& gfx, int x, int y);

enum class Btn : uint8_t { Up = 0, Down, Left, Right, Confirm, Back, COUNT };

class Input {
 public:
  // Hold time that turns a press into a long-press. ~550ms: longer than any
  // deliberate tap, shorter than the SDK's own 650ms confirm-back hold.
  static constexpr unsigned long kLongPressMs = 550;

  // Sticky shared OK/power pin (GPIO4): the one button is BOTH Confirm and
  // power/wake, so the two gesture sets must not overlap. Confirm only ever
  // TAPS from this pin — its long-press is disabled here (a touch long-press
  // on the Confirm soft-key tab supplies that gesture instead, §5.3), and any
  // hold that reaches the nap threshold is a power gesture, not a Confirm.
  // checkPowerButton() (main.cpp) uses the same kStickyNapMinMs to decide nap.
  static constexpr unsigned long kStickyNapMinMs = 1500;   // >= this hold = power, not Confirm
  static constexpr unsigned long kStickyOffMs = 5000;      // release >= this = deep sleep
  static constexpr unsigned long kStickyRestartMs = 10000; // held this long = restart

  void begin() {
    _mgr.begin();  // pinMode + ADC attenuation per BoardConfig
    // The power button's press edge is stamped by an interrupt: the idle
    // sampler runs every 50 ms, and a quick tap used to start late or fall
    // between two samples (2026-09-05). The release is still sampled; the
    // hold is measured from the edge.
    sInstance = this;
    // The press that woke the device from deep sleep is usually still down
    // at boot: ignore the power button until the sampler has seen it
    // released, or that same press reads as a nap tap right after the wake
    // (Andrew, 2026-09-05, 22:15).
    _powerIgnoreUntilUp = true;
    attachInterrupt(digitalPinToInterrupt(InputManager::POWER_BUTTON_PIN), &Input::powerIsr, FALLING);
  }

  // Phase 1: start the dedicated 5 ms sampling task. After this, the SDK
  // manager is owned by that task and update() only drains latched events.
  void beginTask() {
    if (_task) return;
    xTaskCreate(&Input::taskTrampoline, "xp_input", 2560, this, 2, &_task);
  }

  // Stop sampling before deep sleep / restart so the task isn't mid-ADC-read
  // during teardown. (Wake from X3 sleep is a full power-on reset, so there
  // is no resume path — suspend is enough.)
  void suspendTask() {
    detachInterrupt(digitalPinToInterrupt(InputManager::POWER_BUTTON_PIN));
    if (_task) vTaskSuspend(_task);
  }

  // Main-loop tick: drain events latched by the sampling task into this
  // tick's one-shot flags. Without the task (beginTask() not called) it
  // samples inline first — identical semantics, single code path.
  void update() {
    if (!_task) sampleOnce();
    uint8_t tap, lng;
    bool any;
    portENTER_CRITICAL(&_mux);
    tap = _pendingTap;
    lng = _pendingLong;
    any = _pendingAny;
    _pendingTap = 0;
    _pendingLong = 0;
    _pendingAny = false;
    _powerRelThisTick = _powerRelPending;
    _powerRelHeld = _powerRelHeldMs;
    _powerRelPending = false;
    portEXIT_CRITICAL(&_mux);
    for (uint8_t i = 0; i < kBtnCount; i++) {
      _tap[i] = tap & (1u << i);
      _long[i] = lng & (1u << i);
    }
    _anyThisTick = any;
  }

  // TAP: released before kLongPressMs (one-tick event, latched so a tap
  // during a flush fires the moment the loop next drains).
  bool wasPressed(Btn b) const { return _tap[static_cast<uint8_t>(b)]; }
  // LONG-PRESS: fired once at kLongPressMs while still held (one-tick event).
  bool wasLongPressed(Btn b) const { return _long[static_cast<uint8_t>(b)]; }

  // Debounced level, from the sampling task's published snapshot.
  bool isPressed(Btn b) const { return _levelMask & (1u << static_cast<uint8_t>(b)); }
  // Any ladder press edge since the last update() drain (idle-timer reset).
  bool wasAnyPressed() const { return _anyThisTick; }
  // A completed tap or long press on one of the six face buttons this tick.
  // Unlike wasAnyPressed(), the power button is NOT included: its press-down
  // woke the nap and its release then napped it again (Andrew, 2026-09-05).
  bool anyFaceTap() const {
    for (uint8_t i = 0; i < kBtnCount; i++) {
      if (_tap[i] || _long[i]) return true;
    }
    return false;
  }
  // Milliseconds since any ladder button was down or changed (task clock).
  uint32_t msSinceActivity() const { return millis() - _lastActivityMs; }
  // Idle sampling slice (light-sleep builds): the button-check cadence once
  // nothing has been pressed for a second. Also the worst-case press delay.
  static uint32_t& idleSampleMs() {
    static uint32_t ms = 50;
    return ms;
  }
  void noteActivity() { _lastActivityMs = millis(); }

  // Bench dev console (main.cpp pumpDevConsole): inject a synthetic TAP or
  // LONG-PRESS as if the sampling task had latched it — drains through
  // update() exactly like a physical press. The tap/long distinction is the
  // whole input vocabulary: scenes only ever consume the one-shot edges, so
  // these two cover every ladder-button behavior (a real long-press also
  // suppresses its release-tap, which injection matches by setting only one
  // bit). Guarded by USB-host presence at the call site.
  // Bench-only: hold `b` down for `ms`, as if a thumb were on it. Needed
  // because injectTap only latches an edge — nothing in the tap/long-press
  // vocabulary can express "still held", which is what auto-repeat reads.
  void injectHold(Btn b, uint32_t ms) {
    portENTER_CRITICAL(&_mux);
    _holdInjectMask |= (1u << static_cast<uint8_t>(b));
    _holdInjectUntilMs = millis() + ms;
    _pendingAny = true;
    portEXIT_CRITICAL(&_mux);
  }

  void injectTap(Btn b) {
    portENTER_CRITICAL(&_mux);
    _pendingTap |= (1u << static_cast<uint8_t>(b));
    _pendingAny = true;
    portEXIT_CRITICAL(&_mux);
  }
  void injectLong(Btn b) {
    portENTER_CRITICAL(&_mux);
    _pendingLong |= (1u << static_cast<uint8_t>(b));
    _pendingAny = true;
    portEXIT_CRITICAL(&_mux);
  }

  // Power button (digital pin per BoardConfig input.power — GPIO3 on X3/X4,
  // active-low; the SDK debounces it alongside the ladder buttons). Published
  // by the sampling task; single-word volatile reads are atomic on the C3.
  bool powerPressed() const { return _powerDown; }
  unsigned long powerHeldMs() const { return _powerHeldMs; }
  // A completed power press this tick, with the hold the task measured. The
  // release is latched by the 5 ms task, so a quick tap can no longer fall
  // between the 50 ms idle slices of the main loop (2026-09-05).
  bool powerReleased(unsigned long& heldMs) const {
    if (!_powerRelThisTick) return false;
    heldMs = _powerRelHeld;
    return true;
  }
  // Bench: a synthetic power press of `ms` (the task reads it as the pin).
  void injectPower(uint32_t ms) {
    _powerInjectUntilMs = millis() + ms;
    _powerEdgeMs = millis();
    _powerEdgeSeen = true;
  }

 private:
  static constexpr uint8_t kBtnCount = static_cast<uint8_t>(Btn::COUNT);
  volatile uint32_t _lastActivityMs = 0;

  // Fixed logical -> SDK physical index map.
  static constexpr uint8_t kMap[kBtnCount] = {
      InputManager::BTN_UP,    InputManager::BTN_DOWN,    InputManager::BTN_LEFT,
      InputManager::BTN_RIGHT, InputManager::BTN_CONFIRM, InputManager::BTN_BACK,
  };

  static void taskTrampoline(void* self) { static_cast<Input*>(self)->taskLoop(); }

  static Input* sInstance;
  static void IRAM_ATTR powerIsr() {
    if (!sInstance) return;
    sInstance->_powerEdgeMs = millis();
    sInstance->_powerEdgeSeen = true;
    sInstance->_lastActivityMs = millis();  // sampling returns to 5 ms at once
  }

  [[noreturn]] void taskLoop() {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
#if XP_LIGHT_SLEEP_LIBS
      // P4: 5 ms while anything is pressed or was within the last second,
      // 50 ms slices once idle — a 5 ms wake cadence never leaves the idle
      // task the 3 ticks it needs to enter light sleep. Worst case the first
      // press after idle registers ~50 ms late (Andrew: acceptable).
      const TickType_t period = pdMS_TO_TICKS(msSinceActivity() > 1000 ? idleSampleMs() : 5);
      vTaskDelayUntil(&last, period);
#else
      vTaskDelayUntil(&last, pdMS_TO_TICKS(5));
#endif
      sampleOnce();
    }
  }

  // Touch → button routing (Sticky). Runs on the sampling task right after
  // _mgr.update() so the GT911 events are fresh. A tap on a soft-key tab
  // injects that logical button; a horizontal swipe injects Left/Right.
  // Compiled only on touch boards; inert otherwise.
  void serviceTouchButtons(unsigned long now) {
#if FREEINK_CAP_TOUCH
    if (!_mgr.hasTouch()) return;
    // Panel-native (0..1) -> native px -> LOGICAL px. drawPixel()'s portrait
    // transform (Gfx.cpp: phyX = y; phyY = logicalW - 1 - x) inverts to:
    //   logicalX = logicalW - 1 - nativeY;   logicalY = nativeX
    // where logicalW = native panel HEIGHT. In Gfx Landscape (reader) the
    // transform is the identity.
    const int natW = BoardConfig::ACTIVE.displayWidth;
    const int natH = BoardConfig::ACTIVE.displayHeight;
    auto toLogical = [&](float nx, float ny, int& lx, int& ly) {
      const int px = static_cast<int>(nx * natW);
      const int py = static_cast<int>(ny * natH);
      if (G_GFX && G_GFX->orientation() == Gfx::Orient::Portrait) {
        lx = natH - 1 - py;
        ly = px;
      } else {
        lx = px;
        ly = py;
      }
    };
    float nx, ny;
    if (_mgr.wasTouchTap(nx, ny)) {
      // A touch that already fired its long-press does NOT also tap on release
      // (mirrors the physical machine: a long-press consumes the hold).
      const bool wasLong = _touchLongFired;
      _touchLongFired = false;  // contact ended; rearm for the next one
      if (!wasLong) {
        int lx, ly;
        toLogical(nx, ny, lx, ly);
        const int slot = softKeySlotAt(*G_GFX, lx, ly);
        if (slot >= 0) {
          // Soft-key slot order is Back=0, Confirm=1, Left=2, Right=3 — the same
          // fixed map as kMap. Route through the same latch as a physical press.
          static constexpr Btn kSlotBtn[4] = {Btn::Back, Btn::Confirm, Btn::Left, Btn::Right};
          injectTap(kSlotBtn[slot]);
        }
      }
      _lastActivityMs = now;
      return;  // a tap is never also a swipe
    }
    // Touch LONG-PRESS on a tab: a held finger still within tap slop past
    // kLongPressMs injects that button's long-press (this is how Confirm-long
    // — bookmark delete, clear-all — is reached on the Sticky, since the
    // physical OK pin's own long-press is reserved for power). Fires once per
    // contact; the release then produces no tap (the hold consumed it).
    unsigned long heldMs = 0;
    if (!_touchLongFired && _mgr.isTouchTapCandidate(nx, ny, heldMs) && heldMs >= kLongPressMs) {
      int lx, ly;
      toLogical(nx, ny, lx, ly);
      const int slot = softKeySlotAt(*G_GFX, lx, ly);
      if (slot >= 0) {
        static constexpr Btn kSlotBtn[4] = {Btn::Back, Btn::Confirm, Btn::Left, Btn::Right};
        injectLong(kSlotBtn[slot]);
        _touchLongFired = true;
      }
      _lastActivityMs = now;
    }
    if (!_mgr.isTouchPressed()) _touchLongFired = false;  // rearm on lift
    float sx0, sy0, sx1, sy1;
    if (_mgr.wasSwipe(sx0, sy0, sx1, sy1)) {
      // Horizontal swipes map to Left/Right in the native frame's x axis
      // (native -x = logical left, +x = logical right after the rotation).
      const float dxn = sx1 - sx0;
      constexpr float kSwipeMin = 0.06f;  // ~48 px on the 800px native axis
      if (dxn <= -kSwipeMin) injectTap(Btn::Left);
      else if (dxn >= kSwipeMin) injectTap(Btn::Right);
      _lastActivityMs = now;
    }
#else
    (void)now;
#endif
  }

  // One SDK sample + tap/long-press machine pass; results latch into the
  // pending masks (OR-accumulated until the main loop drains them).
  void sampleOnce() {
    _mgr.update();
    const unsigned long now = millis();
    uint8_t tapBits = 0, longBits = 0, levels = 0;
    for (uint8_t i = 0; i < kBtnCount; i++) {
      const bool down = _mgr.isPressed(kMap[i]);
      if (down) levels |= (1u << i);
      if (down) _lastActivityMs = now;
      // Sticky shared OK/power pin: the Confirm button is GPIO4, the power
      // button's pin. A hold that reaches the nap threshold is a POWER gesture,
      // not a Confirm — suppress the pending tap and never fire Confirm-long
      // from this pin (touch supplies Confirm-long on the soft-key tab).
      const bool sharedPowerConfirm =
          BoardConfig::isSticky() && i == static_cast<uint8_t>(Btn::Confirm);
      if (down) {
        if (!_held[i]) {  // press edge: start the hold clock
          _held[i] = true;
          _longFired[i] = false;
          _pressStartMs[i] = now;
        } else if (!_longFired[i] && !sharedPowerConfirm && now - _pressStartMs[i] >= kLongPressMs) {
          _longFired[i] = true;  // fire-once while held, no repeat
          longBits |= (1u << i);
        }
      } else if (_held[i]) {  // release edge
        _held[i] = false;
        const unsigned long heldMs = now - _pressStartMs[i];
        // On the shared pin, a hold that reached the nap threshold belongs to
        // the power path — swallow the tap so release doesn't ALSO Confirm.
        const bool powerOwns = sharedPowerConfirm && heldMs >= kStickyNapMinMs;
        if (!_longFired[i] && !powerOwns) tapBits |= (1u << i);  // short hold -> tap
        // else: the long-press (or the power gesture) consumed this hold — no tap.
      }
    }
    const bool any = _mgr.wasAnyPressed();
    // Touch (Sticky): a tap on a soft-key tab injects that button; a horizontal
    // swipe injects Left/Right. Both reuse the same latch path as a physical
    // press, so every scene works unchanged. Touch counts as activity so the
    // idle/nap timers reset.
    serviceTouchButtons(now);
    portENTER_CRITICAL(&_mux);
    _pendingTap |= tapBits;
    _pendingLong |= longBits;
    if (any) _pendingAny = true;
    portEXIT_CRITICAL(&_mux);
    // Bench: a synthetic HOLD (see injectHold) ORs into the published level
    // for its window, so level-driven behaviour — the chapter list's
    // hold-to-jump, say — is testable from the dev console. Real presses are
    // never suppressed; this only ever adds bits.
    uint8_t injected = 0;
    if (_holdInjectMask && static_cast<int32_t>(millis() - _holdInjectUntilMs) < 0) {
      injected = _holdInjectMask;
    } else {
      _holdInjectMask = 0;
    }
    _levelMask = levels | injected;
    // Power button, task-side: counts as activity (so sampling returns to 5 ms
    // at once), its hold runs on our own clock, and the release edge is
    // latched with that hold for the main loop to take.
    bool pdown = _mgr.isPressed(InputManager::BTN_POWER);
    if (_powerInjectUntilMs) {
      if (static_cast<int32_t>(millis() - _powerInjectUntilMs) < 0) pdown = true;
      else _powerInjectUntilMs = 0;
    }
    // The interrupt fires on every falling edge, bounces included. A bounce
    // on RELEASE used to read as a second press (Andrew, 2026-09-05: "tap to
    // wake and it goes right back to sleep"), so edges inside a short quiet
    // window after a release are ignored, and a press the interrupt saw but
    // the sampler never did (a quick tap between samples) counts as a
    // nominal tap rather than as the sampling delay.
    // One physical press must be ONE press. The interrupt sees it first; the
    // SDK's debounce reports the pin low a few samples later, while the
    // finger is still down. So a press the interrupt started stays open
    // until the sampler sees the real release; only if the sampler never
    // saw the pin low at all (a very quick tap) does it close by itself
    // after a short grace, as a nominal tap. (Andrew, 2026-09-05: one tap
    // read as nap + wake, "it flashes back awake".)
    constexpr unsigned long kPowerQuietMs = 250;
    constexpr unsigned long kEdgeGraceMs = 150;
    constexpr unsigned long kEdgeOnlyTapMs = 40;
    bool edge = _powerEdgeSeen;
    _powerEdgeSeen = false;
    if (_powerIgnoreUntilUp) {
      if (pdown) { _lastActivityMs = now; _powerDown = true; return; }  // still the wake press
      _powerIgnoreUntilUp = false;
      _powerLastReleaseMs = now;  // and a quiet window after it
      edge = false;
    }
    if (edge && !_powerHeld && now - _powerLastReleaseMs >= kPowerQuietMs) {
      _powerHeld = true;
      _powerEdgeOnly = true;
      _powerPressStartMs = _powerEdgeMs;
    }
    bool completed = false;
    unsigned long held = 0;
    if (pdown) {
      _lastActivityMs = now;
      if (!_powerHeld) {
        _powerHeld = true;
        _powerPressStartMs = now;
      }
      _powerEdgeOnly = false;  // the sampler saw the pin low: this press ends on its real release
      _powerHeldMs = now - _powerPressStartMs;
    } else if (_powerHeld) {
      if (_powerEdgeOnly) {
        if (now - _powerPressStartMs >= kEdgeGraceMs) {  // the sampler never saw it: a quick tap
          completed = true;
          held = kEdgeOnlyTapMs;
        }
      } else {
        completed = true;
        held = now - _powerPressStartMs;
      }
    }
    if (completed) {
      _powerHeld = false;
      _powerEdgeOnly = false;
      _powerLastReleaseMs = now;
      portENTER_CRITICAL(&_mux);
      _powerRelHeldMs = held;
      _powerRelPending = true;
      portEXIT_CRITICAL(&_mux);
    }
    _powerDown = pdown;
  }

  InputManager _mgr;  // task-owned once beginTask() runs
  TaskHandle_t _task = nullptr;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

  // Tap/long-press machine state (sampling context only).
  unsigned long _pressStartMs[kBtnCount] = {};
  bool _held[kBtnCount] = {};
  bool _powerHeld = false;
  unsigned long _powerPressStartMs = 0;
  volatile bool _powerRelPending = false;
  volatile unsigned long _powerRelHeldMs = 0;
  bool _powerRelThisTick = false;
  unsigned long _powerRelHeld = 0;
  volatile uint32_t _powerInjectUntilMs = 0;
  volatile unsigned long _powerEdgeMs = 0;
  volatile bool _powerEdgeSeen = false;
  bool _powerEdgeOnly = false;
  bool _powerIgnoreUntilUp = false;
  unsigned long _powerLastReleaseMs = 0;
  bool _longFired[kBtnCount] = {};
  bool _touchLongFired = false;  // one touch long-press per contact (Sticky)

  // Latches: sampling context -> main loop (guarded by _mux).
  uint8_t _pendingTap = 0;
  uint8_t _pendingLong = 0;
  bool _pendingAny = false;

  // Published snapshots (volatile single-word, lock-free reads).
  volatile uint8_t _levelMask = 0;
  volatile uint8_t _holdInjectMask = 0;   // bench hold injection (see injectHold)
  volatile uint32_t _holdInjectUntilMs = 0;
  volatile bool _powerDown = false;
  volatile unsigned long _powerHeldMs = 0;

  // This tick's drained one-shot events (main loop only).
  bool _tap[kBtnCount] = {};
  bool _long[kBtnCount] = {};
  bool _anyThisTick = false;
};
