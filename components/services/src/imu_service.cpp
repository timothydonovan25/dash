// `imu` — core 0, priority 15, 200 Hz.
//
// Drains the FIFO, runs every sample through the attitude filter and the
// jump detector, publishes one Attitude. Both of those are core modules with
// host tests behind them; this file is a loop and a health report.
#include "services/services.hpp"
#include "core/attitude/attitude.hpp"
#include "core/jump/jump.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

namespace services {
namespace {

constexpr size_t kBatch = 16;

void imu_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);

    core::attitude::Filter   filter;
    core::attitude::Config   acfg;
    core::jump::Detector     jumps;
    core::jump::Config       jcfg;

    filter.set_mount_offset(d.settings->imu_roll_offset_deg,
                            d.settings->imu_pitch_offset_deg);

    // Zero the gyro against a stationary bike. If the rider is already
    // moving this is wrong, so it is only taken while the readings are
    // consistent with sitting still.
    {
        hal::IImu::Sample s[kBatch];
        size_t n = 0;
        double gx = 0, gy = 0, gz = 0;
        size_t taken = 0;
        for (int i = 0; i < 40 && taken < 200; ++i) {
            if (d.imu && hal::ok(d.imu->read_fifo(s, kBatch, n))) {
                for (size_t k = 0; k < n; ++k) {
                    const float mag = std::sqrt(s[k].ax_g * s[k].ax_g +
                                                s[k].ay_g * s[k].ay_g +
                                                s[k].az_g * s[k].az_g);
                    if (std::fabs(mag - 1.0f) > 0.05f) continue;   // moving
                    gx += s[k].gx_dps;
                    gy += s[k].gy_dps;
                    gz += s[k].gz_dps;
                    ++taken;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (taken > 50) {
            filter.set_gyro_bias(static_cast<float>(gx / taken),
                                 static_cast<float>(gy / taken),
                                 static_cast<float>(gz / taken));
        }
    }

    for (;;) {
        hal::IImu::Sample batch[kBatch];
        size_t n = 0;
        const hal::Status s = d.imu ? d.imu->read_fifo(batch, kBatch, n)
                                    : hal::Status::NotPresent;
        const uint32_t now = d.clock->now_ms();

        if (hal::ok(s)) {
            if (n > 0) d.health->report_ok(core::state::Dev::Imu, now);

            const float speed = d.slots->motion.get().best_mph;
            for (size_t i = 0; i < n; ++i) {
                core::attitude::Sample a;
                a.gx_dps = batch[i].gx_dps;
                a.gy_dps = batch[i].gy_dps;
                a.gz_dps = batch[i].gz_dps;
                a.ax_g = batch[i].ax_g;
                a.ay_g = batch[i].ay_g;
                a.az_g = batch[i].az_g;
                a.speed_mph = speed;      // the v.omega reference needs it
                a.t_ms = batch[i].t_ms;
                filter.update(a, acfg);

                const float mag = std::sqrt(a.ax_g * a.ax_g + a.ay_g * a.ay_g +
                                            a.az_g * a.az_g);
                jumps.update(mag, batch[i].t_ms, jcfg);
            }

            core::state::Attitude out;
            filter.publish_into(out);
            jumps.publish_into(out);
            d.slots->attitude.publish(out);
        } else {
            d.health->report_error(core::state::Dev::Imu,
                                   static_cast<int32_t>(s), now);
        }

        // 5 ms is the 200 Hz sample period. The FIFO means a late tick costs
        // latency, not data — which is why this task can sit below `ctrl`.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void start_imu(Deps& d) {
    xTaskCreatePinnedToCore(imu_task, "imu", 4096, &d, 15, nullptr, 0);
}

}  // namespace services
