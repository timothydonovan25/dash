canvas/ widgets/ screens/ and the render task.

A screen is a pure function of one Frame: update() sets widget values and
must not draw. The render task repaints whatever went dirty.

Start here before the hardware arrives — screens built against a fake
Frame are the highest-value work available right now.
