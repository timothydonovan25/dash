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

// The longest `gnss` waits for a byte. It wakes as soon as one arrives (the
// UART's interrupt, through the driver's ring buffer); this only bounds how
// late a dead receiver's fix is aged out and a retry is picked up.
constexpr uint32_t kWaitMs = 50;

// The receiver's set-up (Pa1616d::init: a few seconds at most), and what it
// came to: a fault if nothing answered, Degraded if it is stuck at 1 Hz
// (limited), Ok at 10 Hz. On a retry too, which used to ignore the result.
void bring_up(Deps& d) {
    const hal::Status s = d.gnss->init();
    const uint32_t now = d.clock->now_ms();
    if (hal::ok(s)) {
        d.health->report_limited(core::state::Dev::Gnss, !d.gnss->full_rate());
        d.health->report_ok(core::state::Dev::Gnss, now);
    } else {
        d.health->report_error(core::state::Dev::Gnss, static_cast<int32_t>(s), now);
    }
}

void gnss_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::nmea::Parser parser;
    core::state::Gnss  fix;

    // The receiver is brought up here, not in wiring::build(): agreeing a
    // baud rate takes seconds of listening, and nothing else in the boot
    // needs the GPS. A missing receiver fails here and the keeper retries it.
    if (d.gnss) {
        d.health->begin_init(core::state::Dev::Gnss, d.clock->now_ms());
        bring_up(d);
    }

    for (;;) {
        // A re-init `keeper` asked for. Here, because it changes the UART's
        // baud rate: from another task it did so under this loop's reads.
        // A sentence cut in half by it fails its checksum and the parser
        // picks up again at the next '$'.
        if (take_retry(d, core::state::Dev::Gnss) && d.gnss) bring_up(d);

        char buf[256];
        size_t n = 0;
        const hal::Status s = d.gnss ? d.gnss->read(buf, sizeof buf, n, kWaitMs)
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
        // fix, and only the first is a fault: `report_ok` above is what keeps
        // the health tracker's staleness timer fed. Either way its last
        // position is not a fix any more, for every reader of the slot.
        if (core::nmea::age_fix(fix, now)) {
            d.slots->gnss.publish(fix);
            wake_logic(d);
        }

        // No sleep on success: the read above is the wait, and it returns
        // when bytes arrive, so a fix is stamped within ~1.5 ms of its
        // sentence ending (the lap timer interpolates crossings from that
        // stamp). A read that failed did not wait, so this one does.
        if (!hal::ok(s)) vTaskDelay(pdMS_TO_TICKS(kWaitMs));
    }
}

}  // namespace

void start_gnss(Deps& d) {
    xTaskCreatePinnedToCore(gnss_task, "gnss", 4096, &d, 12, nullptr, 0);
}

}  // namespace services
