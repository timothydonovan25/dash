// `gnss` — core 0, priority 12.
//
// Reads bytes and feeds core/nmea. That is the whole task: the parser is
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

    for (;;) {
        char buf[256];
        size_t n = 0;
        const hal::Status s = d.gnss ? d.gnss->read(buf, sizeof buf, n)
                                     : hal::Status::NotPresent;
        const uint32_t now = d.clock->now_ms();

        if (hal::ok(s)) {
            bool updated = false;
            for (size_t i = 0; i < n; ++i) {
                if (parser.feed(buf[i], fix) == core::nmea::Result::Updated) {
                    updated = true;
                }
            }
            if (updated) {
                d.slots->gnss.publish(fix);
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

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

void start_gnss(Deps& d) {
    xTaskCreatePinnedToCore(gnss_task, "gnss", 4096, &d, 12, nullptr, 0);
}

}  // namespace services
