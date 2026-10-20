// `imu` — core 0, priority 15, 200 Hz.
//
// Drains the FIFO, runs every sample through the attitude filter and the
// jump detector, publishes one Attitude. Both of those are core modules with
// host tests behind them; this file is a loop and a health report.
#include "services/services.hpp"
#include "core/attitude/attitude.hpp"
#include "core/fixtime/fixtime.hpp"
#include "core/jump/jump.hpp"

#include "bsp/pins.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

namespace services {
namespace {

constexpr const char* kTag = "imu";

constexpr size_t kBatch = 16;
// The FIFO's 2 kB is eight batches of 16-byte packets; one more for the
// samples that land while it drains.
constexpr int kMaxBatchesPerWake = 9;

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

    // Static, not a local: with its heading history it is ~0.5 kB, and
    // the task's frame is held to 1 kB (CLAUDE.md, 1.28). One imu task.
    static core::attitude::Filter filter;
    core::attitude::Config   acfg;
    core::jump::Detector     jumps;
    core::jump::Config       jcfg;
    // Its own, so the slide check compares the gyro with when each fix was
    // taken rather than when it arrived. `logic` keeps another for laps.
    core::fixtime::Clock     fix_clock;
    uint32_t                 last_fix_rx_ms = 0;

    // The mount offset the filter is using. Settings is `logic`'s, so a
    // change -- typed into CONFIG, from the Wi-Fi page, or this task's own
    // calibration coming back -- is seen by comparing, once a wake.
    float mount_roll = d.settings->imu_roll_offset_deg;
    float mount_pitch = d.settings->imu_pitch_offset_deg;
    filter.set_mount_offset(mount_roll, mount_pitch);
    // Static for the frame's sake too, both of them (~0.1 kB each).
    static core::attitude::LevelCal level;
    uint32_t level_gen = d.slots->imu_request.generation();
    // After a calibration, until `logic` has applied its two edits: the old
    // values still in Settings are not a change to go back to.
    uint32_t level_wait_until_ms = 0;

    // Zero the gyro against a stationary bike: a still second, by the same
    // test Calibrate level uses (core::attitude::LevelCal), taken from the
    // main loop's own samples so the lean is live meanwhile. A run that
    // moves starts again, until kBootBiasMs; a bike that never sits still
    // that long keeps a zero bias rather than a wrong one. This used to
    // average whatever came in the first 0.4 s with only the accelerometer
    // checked, and the IMU is on the bars: swinging them while getting on
    // went into the bias, a yaw-rate error that reads as lean all ride.
    constexpr uint32_t kBootBiasMs = 10000;
    const uint32_t boot_ms = d.clock->now_ms();
    static core::attitude::LevelCal boot_bias;
    boot_bias.start();

    g_imu_task = xTaskGetCurrentTaskHandle();
    float imu_temp_c = 0.0f;
    const bool have_int = attach_data_ready();

    watchdog_join();
    for (;;) {
        watchdog_feed();
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

        // A re-init `keeper` asked for, here between FIFO reads rather than
        // from core 1 in the middle of one.
        if (take_retry(d, core::state::Dev::Imu) && d.imu) d.imu->init();

        const bool level_landing =
            static_cast<int32_t>(level_wait_until_ms - d.clock->now_ms()) > 0;
        if ((d.settings->imu_roll_offset_deg != mount_roll ||
             d.settings->imu_pitch_offset_deg != mount_pitch) && !level_landing) {
            mount_roll = d.settings->imu_roll_offset_deg;
            mount_pitch = d.settings->imu_pitch_offset_deg;
            filter.set_mount_offset(mount_roll, mount_pitch);
            filter.reset();      // what it had was in the old axes
        }
        const uint32_t gen = d.slots->imu_request.generation();
        if (gen != level_gen) {
            level_gen = gen;
            if (d.slots->imu_request.get().op ==
                core::state::ImuRequest::CalibrateLevel) {
                level.start();
            }
        }

        // Read until the FIFO is drained, not one batch per wake. After a
        // stall -- SPI2 held by the SD card's busy time, a flash write --
        // there can be several batches waiting, and one per 5 ms wake fell
        // further behind with every one. Bounded: the FIFO holds 2 kB, eight
        // batches of 16, and a part that keeps saying "full" is not allowed
        // to keep this task here.
        for (int drain = 0; drain < kMaxBatchesPerWake; ++drain) {
            hal::IImu::Sample batch[kBatch];
            size_t n = 0;
            const hal::Status s = d.imu ? d.imu->read_fifo(batch, kBatch, n)
                                        : hal::Status::NotPresent;
            const uint32_t now = d.clock->now_ms();

            if (hal::ok(s)) {
                if (n > 0) d.health->report_ok(core::state::Dev::Imu, now);

                // The fused speed, which already refuses a spinning or
                // locked wheel. The speedo's figure is front wheel first,
                // with no slip check: a locked front read the lean low and,
                // with the front dead, wheelspin read it high.
                const core::state::Fused fu = d.slots->fused.get();
                const float speed = fu.speed_valid
                                        ? fu.speed_mph
                                        : d.slots->motion.get().best_mph;
                // Every sample as the part gave it, for the detail log
                // (core/log/detail.hpp): sensor axes, before the mount
                // rotation and the bias, so the filter can be re-run.
                core::log::ImuRing* raw_log =
                    d.settings->detail_log ? d.imu_log : nullptr;
                for (size_t i = 0; i < n; ++i) {
                    if (raw_log) {
                        core::log::ImuRow r;
                        r.t_ms = batch[i].t_ms;
                        r.ax_g = batch[i].ax_g;
                        r.ay_g = batch[i].ay_g;
                        r.az_g = batch[i].az_g;
                        r.gx_dps = batch[i].gx_dps;
                        r.gy_dps = batch[i].gy_dps;
                        r.gz_dps = batch[i].gz_dps;
                        r.temp_c = batch[i].temp_c;
                        r.batch_n = i == 0 ? static_cast<uint8_t>(n) : 0;
                        r.drain = static_cast<uint8_t>(drain);
                        r.chip_ts = batch[i].chip_ts;
                        r.fifo_hdr = batch[i].fifo_hdr;
                        raw_log->push(r);
                    }
                    core::attitude::Sample a;
                    a.gx_dps = batch[i].gx_dps;
                    a.gy_dps = batch[i].gy_dps;
                    a.gz_dps = batch[i].gz_dps;
                    a.ax_g = batch[i].ax_g;
                    a.ay_g = batch[i].ay_g;
                    a.az_g = batch[i].az_g;
                    a.speed_mph = speed;      // the v.omega reference needs it
                    a.t_ms = batch[i].t_ms;

                    // Calibrate level: a still second, then the offsets go
                    // to `logic` as edits, like any other. The filter takes
                    // them now; the compare above then finds them equal.
                    if (boot_bias.active()) {
                        switch (boot_bias.feed(a)) {
                            case core::attitude::LevelCal::Result::Done: {
                                const core::attitude::Sample& m = boot_bias.mean();
                                filter.set_gyro_bias(m.gx_dps, m.gy_dps, m.gz_dps);
                                break;
                            }
                            case core::attitude::LevelCal::Result::Moved:
                                if (a.t_ms - boot_ms < kBootBiasMs) boot_bias.start();
                                break;
                            default:
                                break;
                        }
                    }

                    switch (level.feed(a)) {
                        case core::attitude::LevelCal::Result::Done: {
                            const core::attitude::Sample& m = level.mean();
                            float roll = 0.0f, pitch = 0.0f;
                            filter.calibrate_level(m, roll, pitch);
                            filter.set_gyro_bias(m.gx_dps, m.gy_dps, m.gz_dps);
                            boot_bias = core::attitude::LevelCal{};   // this one is newer
                            mount_roll = roll;
                            mount_pitch = pitch;
                            // logic applies them within a pass; a second is
                            // plenty, and a dropped edit is then undone
                            // rather than left disagreeing with NVS.
                            level_wait_until_ms = a.t_ms + 1000;
                            if (d.commands) {
                                d.commands->post(core::state::Command::config_set(
                                    core::config::kKeyImuRollOffset, roll));
                                d.commands->post(core::state::Command::config_set(
                                    core::config::kKeyImuPitchOffset, pitch));
                            }
                            ESP_LOGI(kTag, "level: roll %.2f pitch %.2f deg",
                                     static_cast<double>(roll), static_cast<double>(pitch));
                            break;
                        }
                        case core::attitude::LevelCal::Result::Moved:
                            ESP_LOGW(kTag, "level refused: the bike moved");
                            break;
                        default:
                            break;
                    }

                    filter.update(a, acfg);

                    const float mag = std::sqrt(a.ax_g * a.ax_g + a.ay_g * a.ay_g +
                                                a.az_g * a.az_g);
                    jumps.update(mag, batch[i].t_ms, jcfg);
                }

                // A new fix, after the samples up to its arrival, so the
                // filter's heading history covers when it was taken.
                const core::state::Gnss g = d.slots->gnss.get();
                if (g.rx_ms != last_fix_rx_ms) {
                    last_fix_rx_ms = g.rx_ms;
                    const uint32_t taken =
                        g.fix ? fix_clock.stamp(g.utc_ms, g.rx_ms) : g.rx_ms;
                    filter.on_fix(g.course_deg, g.speed_mph, taken, g.fix, acfg);
                }

                if (n > 0) imu_temp_c = batch[n - 1].temp_c;
                core::state::Attitude out;
                filter.publish_into(out);
                jumps.publish_into(out);
                out.imu_temp_c = imu_temp_c;
                d.slots->attitude.publish(out);
                if (n < kBatch) break;          // drained
            } else {
                d.health->report_error(core::state::Dev::Imu,
                                       static_cast<int32_t>(s), now);
                break;
            }
        }

    }
}

}  // namespace

void start_imu(Deps& d) {
    xTaskCreatePinnedToCore(imu_task, "imu", 4096, &d, 15, nullptr, 0);
}

}  // namespace services
