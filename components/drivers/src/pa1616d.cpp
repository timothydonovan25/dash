#include "drivers/pa1616d.hpp"
#include "bsp/pins.hpp"
#include "core/nmea/nmea.hpp"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace drivers {
namespace {
constexpr uart_port_t kPort = UART_NUM_1;

// After the baud command, before listening at the new rate.
constexpr uint32_t kSwitchMs = 100;
// Long enough to hear a receiver still at its 1 Hz default.
constexpr uint32_t kListenMs = 1500;
// A PMTK001 comes back within a few sentences.
constexpr uint32_t kAckMs = 500;
// GLL, RMC, VTG, GGA, GSA, GSV, then reserved and MCHN (PMTK314's order).
constexpr const char* kOutputRmcGga = "PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";
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
    full_rate_ = false;

    // Which rate the receiver is at is not known here. Every power-up is
    // 9600 and 1 Hz: VBACKUP is tied to VCC, and the PMTK manual says a
    // setting outlives power only on backup. 57600 at 10 Hz if the P4 alone
    // restarted (a panic, the watchdog). Either, after the module alone
    // reset. So the baud command goes out at both rates: the receiver hears
    // the one sent at its own and takes the other for line noise. It does
    // not acknowledge PMTK251, so what proves it is the listening below.
    for (const int b : {bsp::kGpsRunBaud, bsp::kGpsBootBaud}) {
        if (!hal::ok(set_baud(b))) return hal::Status::IoError;
        send_command("PMTK251,57600");
        uart_wait_tx_done(kPort, pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(kSwitchMs));

    // A sentence that passes its checksum, at 57600: we agree. Bytes alone
    // proved nothing (bytes from before the switch, or the wrong rate's
    // garbage, passed), and the old 250 ms window missed a 1 Hz receiver
    // most of the time.
    set_baud(bsp::kGpsRunBaud);
    if (!hears_nmea(kListenMs)) {
        set_baud(bsp::kGpsBootBaud);
        if (!hears_nmea(kListenMs)) return hal::Status::NotPresent;
        // Talking at 9600, so it is up and listening now. The first ask may
        // have gone out before it was (it powers up with the P4, and only a
        // Fault is retried, so a missed one is 1 Hz for the whole ride) or
        // been garbled. Ask once more, at the rate it has just proved.
        send_command("PMTK251,57600");
        uart_wait_tx_done(kPort, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(kSwitchMs));
        set_baud(bsp::kGpsRunBaud);
        if (!hears_nmea(kListenMs)) {
            // Would not move. It works, at 1 Hz: 10 Hz of RMC and GGA does
            // not fit in 9600 baud. Degraded, not a fault.
            set_baud(bsp::kGpsBootBaud);
            if (!hears_nmea(kListenMs)) return hal::Status::NotPresent;
            command(kOutputRmcGga, 314, kAckMs);
            return hal::Status::Ok;
        }
    }

    // RMC and GGA only: everything else is bandwidth the parser throws away.
    command(kOutputRmcGga, 314, kAckMs);
    // SBAS works only at 5 Hz or slower (PA1616D datasheet, note 1), so it
    // goes off before asking for 10 Hz.
    command("PMTK313,0", 313, kAckMs);
    // 10 Hz, if the receiver says it is done (flag 3). If not, it stays at
    // its 1 Hz default and full_rate() says so.
    full_rate_ = hal::ok(command("PMTK220,100", 220, kAckMs));
    return hal::Status::Ok;
}

bool Pa1616d::hears_nmea(uint32_t ms) {
    uart_flush_input(kPort);
    core::nmea::LineReader r;
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    char buf[64];
    for (;;) {
        const int32_t left = static_cast<int32_t>(until - xTaskGetTickCount());
        if (left <= 0) return false;
        size_t n = 0;
        if (!hal::ok(read(buf, sizeof buf, n, static_cast<uint32_t>(left)))) return false;
        for (size_t i = 0; i < n; ++i) {
            if (r.feed(buf[i])) return true;
        }
    }
}

hal::Status Pa1616d::command(const char* body, uint16_t cmd, uint32_t ms) {
    const hal::Status s = send_command(body);
    if (!hal::ok(s)) return s;
    core::nmea::LineReader r;
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    char buf[64];
    for (;;) {
        const int32_t left = static_cast<int32_t>(until - xTaskGetTickCount());
        if (left <= 0) return hal::Status::Timeout;
        size_t n = 0;
        if (!hal::ok(read(buf, sizeof buf, n, static_cast<uint32_t>(left)))) return hal::Status::IoError;
        for (size_t i = 0; i < n; ++i) {
            if (!r.feed(buf[i])) continue;
            const int flag = core::nmea::ack_flag(r.line(), cmd);
            if (flag == 3) return hal::Status::Ok;
            if (flag >= 0) return hal::Status::BadReply;   // refused or failed
        }
    }
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
