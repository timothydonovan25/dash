#pragma once
#include "ui/screen.hpp"
#include "services/services.hpp"

// `btn A`..`btn E` / `long A`..`long E` lines on the USB serial port become
// button presses (serial_buttons.cpp; tools/pod_keys.py sends them).
// `crash test` posts to `commands` (features §35). `screen` sends the
// glass (Deps::glass) as text lines for tools/dash_screen.py.
void start_serial_buttons(ui::ICommandSink& commands, services::Deps& deps);
