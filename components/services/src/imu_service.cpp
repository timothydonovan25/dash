// `imu` — core 0, priority 15, 200 Hz.
//
// Drains the FIFO, runs every sample through the attitude filter and the
// jump detector, publishes one Attitude. Both of those are core modules with
// host tests behind them; this file is a loop and a health report.
#include "services/services.hpp"
#include "core/attitude/attitude.hpp"
#include "core/jump/jump.hpp"

#include "bsp/pins.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

namespace services {
namespace {

constexpr size_t kBatch = 16;

// How long to wait for a data-ready edge before reading the FIFO anyway.
// Slightly longer than the 5 ms sample period, so a working interrupt wins
// the race every time and a dead one costs one sample period of latency and
// nothing else. This is the whole safety net for the INT1 register values,
// which have never been checked against a real part.
constexpr uint32_t kFallbackMs = 8;

// Set once, before the task blocks on it.
TaskHandle_t g_imu_task = nullptr;

void IRAM_ATTR drdy_isr(void*) {
    if (!g_imu_task) return;
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(g_imu_task, &woke);
    if (woke) portYIELD_FROM_ISR();
}

// Returns false if the edge could not be hooked up, which is not fatal —
// the wait below simply times out at the poll rate instead.
bool attach_data_ready() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << bsp::kImuInt1;
    io.mode = GPIO_MODE_INPUT;
    io.intr_type = GPIO_INTR_POSEDGE;      // matches INT1 active high
    if (gpio_config(&io) != ESP_OK) return false;

    // engine_sense may already have installed it for the tach; either order
    // has to work, so "already installed" is a success.
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    return gpio_isr_handler_add(static_cast<gpio_num_t>(bsp::kImuInt1),
                                drdy_isr, nullptr) == ESP_OK;
}

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

    g_imu_task = xTaskGetCurrentTaskHandle();
    const bool have_int = attach_data_ready();

    for (;;) {
        // Woken by the part when it has a sample, or by the timeout if the
        // interrupt is not arriving. `imu_timeouts` climbing at the sample
        // rate on the DIAG system screen means INT1 is not working and the
        // task has quietly gone back to polling.
        if (have_int) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFallbackMs)) == 0 &&
                d.system) {
                ++d.system->imu_timeouts;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

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

    }
}

}  // namespace

void start_imu(Deps& d) {
    xTaskCreatePinnedToCore(imu_task, "imu", 4096, &d, 15, nullptr, 0);
}

}  // namespace services
