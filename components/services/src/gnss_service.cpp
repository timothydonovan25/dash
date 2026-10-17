// `gnss` — core 0, priority 12.
//
// Brings the receiver up, then reads bytes and feeds core/nmea. That is the whole task: the parser is
// host-tested against captured sentences, so nothing here has to be clever
// about degrees and minutes.
#include "services/services.hpp"
#include "core/nmea/nmea.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

void gnss_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::nmea::Parser parser;
    core::state::Gnss  fix;
    uint32_t last_fix_ms = 0;

    // The receiver is brought up here, not in wiring::build(): agreeing a
    // baud rate takes half a second of waiting, and nothing else in the boot
    // needs the GPS. A missing receiver fails here and the keeper retries it.
    if (d.gnss) {
        uint32_t now = d.clock->now_ms();
        d.health->begin_init(core::state::Dev::Gnss, now);
        const hal::Status s = d.gnss->init();
        now = d.clock->now_ms();
        if (hal::ok(s)) d.health->report_ok(core::state::Dev::Gnss, now);
        else d.health->report_error(core::state::Dev::Gnss, static_cast<int32_t>(s), now);
    }

    for (;;) {
        // A re-init `keeper` asked for. Here, because it changes the UART's
        // baud rate: from another task it did so under this loop's reads.
        // A sentence cut in half by it fails its checksum and the parser
        // picks up again at the next '$'.
        if (take_retry(d, core::state::Dev::Gnss) && d.gnss) d.gnss->init();

        char buf[256];
        size_t n = 0;
        const hal::Status s = d.gnss ? d.gnss->read(buf, sizeof buf, n)
                                     : hal::Status::NotPresent;
        const uint32_t now = d.clock->now_ms();

        if (hal::ok(s)) {
            bool updated = false;
            for (size_t i = 0; i < n; ++i) {
                const uint32_t utc_before = fix.utc_ms;
                if (parser.feed(buf[i], fix) == core::nmea::Result::Updated) {
                    updated = true;
                    // A new UTC is a new fix; the second sentence of the
                    // same fix carries the same time and keeps the stamp.
                    if (fix.utc_ms != utc_before) fix.rx_ms = now;
                }
            }
            if (updated) {
                d.slots->gnss.publish(fix);
                // The lap timer, the traces and NAV act on a fix the moment
                // it lands, not up to 20 ms later at logic's next tick.
                wake_logic(d);
                last_fix_ms = now;
                d.health->report_ok(core::state::Dev::Gnss, now);
            }
            if (d.system) {
                d.system->gnss_sentences = parser.sentences();
                d.system->gnss_bad_csum = parser.bad_checksum();
            }
        } else {
            d.health->report_error(core::state::Dev::Gnss,
                                   static_cast<int32_t>(s), now);
        }

        // A receiver that has stopped sending is not the same as one with no
        // fix, and only the first is a fault. `report_ok` above is what keeps
        // the health tracker's staleness timer fed.
        (void)last_fix_ms;

        // 5 ms, not 20: the poll is how late a fix is stamped, and the lap
        // timer interpolates line crossings from that stamp. Reading an
        // idle UART is a few microseconds.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void start_gnss(Deps& d) {
    xTaskCreatePinnedToCore(gnss_task, "gnss", 4096, &d, 12, nullptr, 0);
}

}  // namespace services
