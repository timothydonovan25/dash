# dash

Motorcycle dashboard firmware. ESP32-S3, ESP-IDF v6.0.2, FreeRTOS.

Start with `CLAUDE.md`, then `docs/system-architecture.md`.

    # host tests, no ESP-IDF required
    cmake -S test -B build-host && cmake --build build-host
    ctest --test-dir build-host --output-on-failure

    # firmware (EIM install, so activate it first)
    . ~/.espressif/tools/activate_idf_v6.0.2.sh
    idf.py set-target esp32s3 && idf.py build
