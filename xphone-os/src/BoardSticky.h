#pragma once

// xphone-os — reTerminal Sticky board bring-up that has no SDK home.
//
// The Sticky powers off the instant the power button is released unless
// firmware latches the rail: drive GPIO45 HIGH and pulse GPIO46, inside the
// first moments of boot. This is board-support, not SDK (the X3/X4 use the
// opposite mechanism — a GPIO13 battery-latch MOSFET that is OPENED to cut
// power on battery; see Sleep.cpp). No-op on non-Sticky builds.

namespace BoardSticky {

// Latch the main power rail so the device stays on after the power button is
// released. MUST run before anything else in boot() — before serial, before
// the display. Idempotent.
void powerHold();

}  // namespace BoardSticky
