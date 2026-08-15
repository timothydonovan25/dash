# dash

Motorcycle dashboard firmware. ESP32-S3, ESP-IDF v5.x, FreeRTOS.

Start with `CLAUDE.md`, then `docs/system-architecture.md`.

    # host tests, no ESP-IDF required
    cmake -S test -B build-host && cmake --build build-host
    ctest --test-dir build-host --output-on-failure

    # firmware
    idf.py set-target esp32s3 && idf.py build
