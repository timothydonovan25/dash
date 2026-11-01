#pragma once

// `btn A`..`btn E` / `long A`..`long E` lines on the USB serial port become
// button presses (serial_buttons.cpp; tools/pod_keys.py sends them).
void start_serial_buttons();
