#pragma once
#include "state_request.hpp"

namespace demo {

// The five buttons over the USB serial port, for driving the UI from a
// laptop: one line per press, `btn A`..`btn E` short, `long A`..`long E`
// long; `state` prints where the UI is (see state_request.hpp). The touch
// zones keep working alongside it. See serial_buttons.cpp.
void start_serial_buttons(StateRequest& req);

}  // namespace demo
