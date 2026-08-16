canvas/ widgets/ screens/ and the render loop.

A screen is a pure function of one Frame: `update()` sets widget values and
must not draw. The render task repaints whatever went dirty.

Nothing here includes an ESP header, which is deliberate and is enforced by
`test/check_no_esp_headers.sh`. `Canvas` is an interface; `FrameBufferCanvas`
implements it over a plain RGB565 buffer, which is a band in PSRAM on the
device and an array in `test_ui`. That is what makes "does the lap screen
show the right time" a unit test rather than a ride.

All fifteen screens exist and are rendered against a fake Frame by
`test_ui`, including one with nothing published at all — every reading on
that one has to come out `--`.
