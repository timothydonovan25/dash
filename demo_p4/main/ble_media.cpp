#include "ble_media.hpp"
#include "ota_slave.hpp"

#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cinttypes>
#include <cstring>

namespace demo {
namespace {

constexpr const char* kTag = "ble_media";
constexpr const char* kDeviceName = "CRF250F-Dash";

// docs/system-architecture.md §11.1 — "Dash Media". BLE_UUID128_INIT wants
// the bytes in reverse order of the UUID's usual left-to-right string form.
// Service 6e5f0001-b5a3-f393-e0a9-e50e24dcca9e:
static const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x5f, 0x6e);
// Characteristic 6e5f0002-b5a3-f393-e0a9-e50e24dcca9e ("Feed"):
static const ble_uuid128_t kFeedUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x5f, 0x6e);

uint8_t             g_own_addr_type = 0;
SemaphoreHandle_t   g_mutex = nullptr;
core::state::Media  g_media;   // guarded by g_mutex

void set_media(const core::state::Media& m) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_media = m;
    xSemaphoreGive(g_mutex);
}

// One line: tag byte, then the rest of the line is the value (§11.1's
// examples are "T <title>", i.e. tag, one separator byte, value — the
// separator itself is not specified beyond that one example, so this
// treats anything after the tag byte as the value with at most one
// leading space stripped, which reads both "T Foo" and "TFoo" the same).
// §11.1's `D`/`P` values are decimal ASCII milliseconds. No sign, no
// overflow check beyond wrapping like any other uint32_t — a malformed
// value just draws a wrong number, not a crash.
uint32_t parse_u32(const uint8_t* s, uint16_t len) {
    uint32_t v = 0;
    for (uint16_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + static_cast<uint32_t>(s[i] - '0');
    }
    return v;
}

void apply_line(const uint8_t* data, uint16_t len) {
    if (len == 0) return;
    // A write-no-response line may or may not carry a trailing newline;
    // the end of the write is the end of the line either way.
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) --len;
    if (len == 0) return;

    const char tag = static_cast<char>(data[0]);
    const uint8_t* rest = data + 1;
    uint16_t rest_len = len - 1;
    if (rest_len > 0 && rest[0] == ' ') { ++rest; --rest_len; }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool known = true;
    switch (tag) {
        case 'T': {
            const size_t n = rest_len < sizeof(g_media.title) - 1
                                 ? rest_len : sizeof(g_media.title) - 1;
            std::memcpy(g_media.title, rest, n);
            g_media.title[n] = '\0';
            break;
        }
        case 'A': {
            const size_t n = rest_len < sizeof(g_media.artist) - 1
                                 ? rest_len : sizeof(g_media.artist) - 1;
            std::memcpy(g_media.artist, rest, n);
            g_media.artist[n] = '\0';
            break;
        }
        case 'L': {
            const size_t n = rest_len < sizeof(g_media.album) - 1
                                 ? rest_len : sizeof(g_media.album) - 1;
            std::memcpy(g_media.album, rest, n);
            g_media.album[n] = '\0';
            break;
        }
        case 'S':
            if (rest_len > 0) g_media.state = static_cast<uint8_t>(rest[0] - '0');
            break;
        case 'D':
            g_media.duration_ms = parse_u32(rest, rest_len);
            break;
        case 'P':
            // An anchor, not a live value (§11.1) — record the dash's own
            // clock alongside it so the screen can extrapolate forward
            // while playing rather than needing this resent every tick.
            g_media.position_ms = parse_u32(rest, rest_len);
            g_media.position_at_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            break;
        default:
            known = false;   // unknown tags are ignored, not an error (§11.1)
            break;
    }
    if (known) g_media.updated_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    xSemaphoreGive(g_mutex);
}

int feed_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                   struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t buf[128];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) len = sizeof(buf);
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, nullptr) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    apply_line(buf, len);
    return 0;
}

const struct ble_gatt_svc_def kGattSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &kFeedUuid.u,
                .access_cb = feed_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {},
        },
    },
    {},
};

int gap_event(struct ble_gap_event* event, void* arg);

void start_advertising() {
    struct ble_gap_adv_params adv_params = {};
    const char* name = ble_svc_gap_device_name();

    // The phone app scans with a service-UUID filter (§11.1 — it has no
    // other way to tell a dash apart from any other BLE peripheral in
    // range), so the 128-bit service UUID has to be in the *advertising*
    // data, not just discoverable after a GATT connect. A 128-bit UUID
    // plus flags leaves no room for the name in the same 31-byte legacy
    // packet, so the name goes in the scan response instead — the
    // ScanFilter still matches on the primary packet's UUID either way.
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(kTag, "adv_set_fields failed");
        return;
    }

    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.name = reinterpret_cast<const uint8_t*>(name);
    rsp_fields.name_len = static_cast<uint8_t>(std::strlen(name));
    rsp_fields.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp_fields) != 0) {
        ESP_LOGE(kTag, "adv_rsp_set_fields failed");
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &adv_params,
                          gap_event, nullptr) != 0) {
        ESP_LOGE(kTag, "adv_start failed");
        return;
    }
    ESP_LOGI(kTag, "advertising as \"%s\"", name);
}

int gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(kTag, "connected");
            } else {
                ESP_LOGW(kTag, "connect failed: %d", event->connect.status);
                start_advertising();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kTag, "disconnected, reason=%d", event->disconnect.reason);
            // §11.1: a dash still showing forty-minute-old media is worse
            // than one showing nothing, and nothing downstream can tell
            // stale from current unless this task says so.
            set_media(core::state::Media{});
            start_advertising();
            return 0;
        default:
            return 0;
    }
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(kTag, "ensure_addr failed");
        return;
    }
    if (ble_hs_id_infer_auto(0, &g_own_addr_type) != 0) {
        ESP_LOGE(kTag, "id_infer_auto failed");
        return;
    }
    start_advertising();
}

void on_reset(int reason) { ESP_LOGW(kTag, "NimBLE reset, reason=%d", reason); }

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

void ble_media_start() {
    g_mutex = xSemaphoreCreateMutex();

    // Brings the SDIO link to the C6 up (demo_p4/c6_cp/ on the other end)
    // and asks it for its firmware version purely as a link-alive check —
    // if this hangs or errors, nothing downstream will work either.
    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(kTag, "esp_hosted_connect_to_slave failed");
        return;
    }
    esp_hosted_coprocessor_fwver_t fwver;
    if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
        ESP_LOGI(kTag, "C6 coprocessor firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                fwver.major1, fwver.minor1, fwver.patch1);
    } else {
        ESP_LOGW(kTag, "could not read C6 coprocessor firmware version");
    }

    // If the C6's firmware is older than this host expects, push the
    // firmware built alongside this project (demo_p4/c6_cp) to it over the
    // same SDIO link and restart — see ota_slave.cpp. Returns immediately
    // (no-op) once the C6 is already current; restarts the host itself if
    // it just performed an upgrade, so nothing below runs against a stale
    // co-processor.
    ota_slave_if_needed();

    if (esp_hosted_bt_controller_init() != ESP_OK) {
        ESP_LOGE(kTag, "esp_hosted_bt_controller_init failed");
        return;
    }
    if (esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGE(kTag, "esp_hosted_bt_controller_enable failed");
        return;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    if (ble_gatts_count_cfg(kGattSvcs) != 0 || ble_gatts_add_svcs(kGattSvcs) != 0) {
        ESP_LOGE(kTag, "GATT service registration failed");
        return;
    }
    ble_svc_gap_device_name_set(kDeviceName);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(kTag, "started");
}

core::state::Media ble_media_snapshot() {
    core::state::Media out;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    out = g_media;
    xSemaphoreGive(g_mutex);
    return out;
}

}  // namespace demo
