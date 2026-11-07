# dash

Motorcycle dashboard firmware for a 2023 Honda CRF250F. ESP32-P4 + ESP32-C6
(Guition JC-ESP32P4-M3-C6), ESP-IDF v6.0.2, FreeRTOS, C++17.

Start with `CLAUDE.md`: the rules, the traps, and a map of every doc. Then
`docs/features.md` for what the firmware does, or
`docs/system-architecture.md` for why it is shaped this way.

    # host tests, no ESP-IDF required
    cmake -S test -B build-host && cmake --build build-host
    ctest --test-dir build-host --output-on-failure

    # simulator
    cmake -S sim -B build-sim && cmake --build build-sim && python3 sim/dash_sim.py

    # firmware (EIM install, so activate it first)
    . ~/.espressif/v6.0.2/esp-idf/export.sh
    idf.py set-target esp32p4 && idf.py build
