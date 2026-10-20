#include "drivers/pa1616d.hpp"
#include "bsp/pins.hpp"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace drivers {
namespace {
constexpr uart_port_t kPort = UART_NUM_1;
}  // namespace

hal::Status Pa1616d::set_baud(int baud) {
    if (uart_set_baudrate(kPort, baud) != ESP_OK) return hal::Status::IoError;
    baud_ = baud;
    return hal::Status::Ok;
}

hal::Status Pa1616d::init() {
    // UART1 belongs to bsp, which brings it up at the receiver's power-on
    // rate. This driver owns the conversation, not the port.
    installed_ = true;
    baud_ = bsp::kGpsBootBaud;

    // The receiver powers up at 9600 and holds its rate across a warm reset
    // but not a cold one, so the sequence has to work from either. Ask for
    // the faster rate, switch, and if nothing arrives fall back — 10 Hz of
    // RMC and GGA does not fit in 9600 baud, so this matters.
    send_command("PMTK251,57600");
    vTaskDelay(pdMS_TO_TICKS(250));
    set_baud(bsp::kGpsRunBaud);
    vTaskDelay(pdMS_TO_TICKS(250));

    size_t pending = 0;
    uart_get_buffered_data_len(kPort, &pending);
    if (pending == 0) {
        // Nothing at the fast rate: it never took. Go back and stay slow
        // rather than sitting mute.
        set_baud(bsp::kGpsBootBaud);
        uart_flush_input(kPort);
        send_command("PMTK220,1000");        // 1 Hz, all that 9600 will carry
        send_command("PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
        // Ok, not an error: the receiver works, just slowly. A 1 Hz fix
        // costs the lap timer its sub-sample interpolation and is worth
        // seeing, so gnss_service logs the fallback and the DIAG GPS screen
        // shows the rate.
        return hal::Status::Ok;
    }

    // 10 Hz, RMC and GGA only. Everything else is bandwidth the parser will
    // throw away, and at 10 Hz there is not much of it spare.
    send_command("PMTK220,100");
    send_command("PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    return hal::Status::Ok;
}

hal::Status Pa1616d::read(char* out, size_t max, size_t& count, uint32_t wait_ms) {
    count = 0;
    if (!installed_ || max == 0) return hal::Status::NotPresent;
    uint8_t* p = reinterpret_cast<uint8_t*>(out);

    // One byte with the wait, the rest without. uart_read_bytes does not
    // return early with what it has: asked for `max` with a timeout, it
    // waits for all of them. Asked for one, it returns as soon as the
    // driver's interrupt has put anything in the ring buffer, which is what
    // wakes `gnss` when a sentence arrives.
    int n = uart_read_bytes(kPort, p, 1, pdMS_TO_TICKS(wait_ms));
    if (n < 0) return hal::Status::IoError;
    if (n == 0) return hal::Status::Ok;
    count = 1;
    if (max > 1) {
        n = uart_read_bytes(kPort, p + 1, static_cast<uint32_t>(max - 1), 0);
        if (n > 0) count += static_cast<size_t>(n);
    }
    return hal::Status::Ok;
}

hal::Status Pa1616d::send_command(const char* nmea_body) {
    if (!installed_ || !nmea_body) return hal::Status::NotPresent;
    uint8_t sum = 0;
    for (const char* c = nmea_body; *c; ++c) sum ^= static_cast<uint8_t>(*c);
    char line[96];
    const int n = std::snprintf(line, sizeof line, "$%s*%02X\r\n", nmea_body, sum);
    if (n <= 0) return hal::Status::BadReply;
    return uart_write_bytes(kPort, line, static_cast<size_t>(n)) == n
               ? hal::Status::Ok
               : hal::Status::IoError;
}

}  // namespace drivers
