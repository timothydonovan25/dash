#pragma once

namespace demo {

// The five buttons over the USB serial port, for driving the UI from a
// laptop: one line per press, `btn A`..`btn E` short, `long A`..`long E`
// long. The touch zones keep working alongside it. See serial_buttons.cpp.
void start_serial_buttons();

}  // namespace demo
