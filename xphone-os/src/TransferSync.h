#pragma once

// All calls run on the main task. HTTP control polling must only request
// cancellation; server/radio/scene teardown happens after handleClient returns.
namespace transfer_sync {
bool active();
bool memoryReleased();
bool handlingHttp();
void setHandlingHttp(bool active);
void requestCancel();
bool cancelRequested();
bool pollControls();  // defined beside the real input/USB owners in main.cpp
}
