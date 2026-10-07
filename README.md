# dash

Motorcycle dashboard firmware. ESP32-P4 + ESP32-C6 (Guition JC-ESP32P4-M3-C6),
ESP-IDF v6.0.2, FreeRTOS.

Start with `CLAUDE.md`, then `docs/system-architecture.md`.

    # host tests, no ESP-IDF required
    cmake -S test -B build-host && cmake --build build-host
    ctest --test-dir build-host --output-on-failure

    # firmware (EIM install, so activate it first)
    . ~/.espressif/v6.0.2/esp-idf/export.sh
    idf.py set-target esp32p4 && idf.py build
