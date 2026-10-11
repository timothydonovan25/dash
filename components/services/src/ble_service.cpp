// `ble` — core 1, priority 4, event driven (§11.4).
//
// The P4 has no radio. The BLE controller runs on the module's ESP32-C6 and
// is reached over SDIO through ESP-Hosted; NimBLE, the host stack, runs here
// on core 1. `demo_p4/main/ble_media.cpp` is where this first worked, and
// this is that code brought under the five rules.
//
// What this task owns: the Dash Media GATT service (§11.1), and the `media`
// and `ble_diag` slots, which nothing else writes. NimBLE's callbacks run on
// its own host task, so they do no work there: each one copies what
// happened into a queue and returns (§11.8), and this task drains the queue,
// runs `core::media` over the lines and publishes. That keeps the one-writer
// rule literal rather than "one writer, most of the time".
//
// Not here yet: the TPMS observer (§11.2). It waits on a sensor to sniff;
// the decoders are a guess until then, and observer is off in sdkconfig.
//
// One file-scope object, `g_ctx`, is the exception to rule 1 in this
// component. NimBLE's sync and reset callbacks take no argument, so there is
// no other way to reach the queue from them.
#include "services/services.hpp"
#include "c6_firmware.hpp"

#include "core/ble/pairing.hpp"
#include "core/media/feed.hpp"

#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

extern "C" void ble_store_config_init(void);

// core::ble::Peers mirrors NimBLE's bond store, so it must hold all of it.
static_assert(CONFIG_BT_NIMBLE_MAX_BONDS <= core::ble::kMaxPeers,
              "core::ble::kMaxPeers is smaller than the NimBLE bond store");

namespace services {
namespace {

constexpr const char* kTag = "ble";
constexpr const char* kDeviceName = "CRF250F-Dash";

// How often the link itself is sampled for DIAG, and how often the stack and
// the phone are reported alive to the health table.
constexpr uint32_t kDiagPeriodMs = 1000;

// The longest the loop sleeps. A Ctrl press from the handlebars waits at
// most this long to reach the phone.
constexpr uint32_t kIdleWaitMs = 50;

// ESP-Hosted creates its transport and RPC tasks at priority 23 with no core
// affinity, which on this chip means "may run on core 0, above ctrl". There
// is no Kconfig for it, so they are lowered here, below everything on core 0
// (input, the lowest, is 9) and above render (6), so the radio still beats
// the panel on core 1. Unpinned, they can still use core 0's idle time, but
// any real-time task preempts them at once.
constexpr UBaseType_t kHostedPrio = 8;
constexpr const char* kHostedTasks[] = {
    "sdio_rx_buf", "sdio_read", "sdio_process_rx", "sdio_write",
    "rpc_rx", "rpc_tx", "rpc_supp_cb",
};

// §11.1. BLE_UUID128_INIT takes the bytes in reverse of the string form.
// Service 6e5f0001-b5a3-f393-e0a9-e50e24dcca9e
const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x5f, 0x6e);
// Feed, phone to dash, 6e5f0002-...
const ble_uuid128_t kFeedUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x5f, 0x6e);
// Ctrl, dash to phone, notify, 6e5f0003-...
const ble_uuid128_t kCtrlUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x5f, 0x6e);

// What a NimBLE callback hands to this task.
struct Event {
    enum Kind : uint8_t {
        Line, Connect, Disconnect, Subscribe, Synced, Reset, Encrypted,
    };
    Kind     kind;
    uint8_t  len;       // Line: bytes in `data`
    bool     flag;      // Subscribe: notifications on
    uint16_t conn;      // Connect: the handle
    int32_t  code;      // Disconnect / Reset: NimBLE's reason; Encrypted: status
    core::ble::PeerId peer;   // Encrypted: the peer's identity address
    // A tag, a space and the longest field (59 bytes) is 61. Anything past
    // this would be cut by core::media anyway.
    uint8_t  data[64];
};

struct Ctx {
    QueueHandle_t q = nullptr;
    uint8_t  own_addr_type = 0;
    uint16_t ctrl_val_handle = 0;     // filled in by NimBLE at registration
    std::atomic<uint32_t> dropped{0}; // events lost to a full queue
    // Written by the `ble` task, read by store_status() on the host task:
    // whether a new bond may push the oldest out of a full store.
    std::atomic<bool> may_evict{true};
};
Ctx g_ctx;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void post(const Event& e) {
    // Never waits. A host task blocked on this queue would stall the whole
    // stack; losing one line costs one field until the phone's next beat.
    if (xQueueSend(g_ctx.q, &e, 0) != pdTRUE) ++g_ctx.dropped;
}

// --- NimBLE side: runs on the NimBLE host task, copies and returns --------

int feed_access(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    Event e{};
    e.kind = Event::Line;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(e.data)) len = sizeof(e.data);
    if (ble_hs_mbuf_to_flat(ctxt->om, e.data, len, nullptr) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    e.len = static_cast<uint8_t>(len);
    post(e);
    return 0;
}

// Ctrl is notify-only, but NimBLE rejects a characteristic with no access
// callback at registration.
int ctrl_access(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*) {
    return BLE_ATT_ERR_UNLIKELY;
}

const ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kFeedUuid.u,
        .access_cb = feed_access,
        // Encrypted only, which on this dash means bonded: the bond is how
        // it knows the phone (§11.3). An unencrypted write request gets
        // Insufficient Encryption back, which the phone app uses to tell
        // when the link is secure; an unencrypted write command is dropped.
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = &kCtrlUuid.u,
        .access_cb = ctrl_access,
        .flags = BLE_GATT_CHR_F_NOTIFY,   // NimBLE adds the CCCD
        .val_handle = &g_ctx.ctrl_val_handle,
    },
    {},
};

const ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChrs,
    },
    {},
};

int gap_event(ble_gap_event* ev, void*);

void start_advertising() {
    // The phone app scans with a service-UUID filter, so the 128-bit UUID
    // goes in the advertisement itself. With the flags it fills the 31-byte
    // packet, so the name goes in the scan response.
    ble_hs_adv_fields f{};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &kSvcUuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) {
        ESP_LOGE(kTag, "adv fields rejected");
        return;
    }

    const char* name = ble_svc_gap_device_name();
    ble_hs_adv_fields rsp{};
    rsp.name = reinterpret_cast<const uint8_t*>(name);
    rsp.name_len = static_cast<uint8_t>(std::strlen(name));
    rsp.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) {
        ESP_LOGE(kTag, "scan response rejected");
        return;
    }

    ble_gap_adv_params p{};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(g_ctx.own_addr_type, nullptr, BLE_HS_FOREVER,
                                     &p, gap_event, nullptr);
    if (rc != 0) ESP_LOGE(kTag, "advertising failed: %d", rc);
}

int gap_event(ble_gap_event* ev, void*) {
    Event e{};
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status != 0) {
                start_advertising();
                return 0;
            }
            e.kind = Event::Connect;
            e.conn = ev->connect.conn_handle;
            post(e);
            // Ask the phone to encrypt. A bonded phone does so with the key
            // it already has; a new one starts pairing, which is Android's
            // one-time "Pair?" prompt. Whether it may stay is decided once
            // it has, in the `ble` task.
            ble_gap_security_initiate(ev->connect.conn_handle);
            // §11.3: metadata is not latency-critical, and a long interval
            // is radio time handed back to the phone's battery. Units of
            // 1.25 ms, so 100–200 ms; supervision timeout in 10 ms units.
            {
                ble_gap_upd_params up{};
                up.itvl_min = 80;
                up.itvl_max = 160;
                up.latency = 0;
                up.supervision_timeout = 400;
                ble_gap_update_params(ev->connect.conn_handle, &up);
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            e.kind = Event::Disconnect;
            e.code = ev->disconnect.reason;
            post(e);
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE: {
            e.kind = Event::Encrypted;
            e.conn = ev->enc_change.conn_handle;
            e.code = ev->enc_change.status;
            ble_gap_conn_desc desc;
            if (ble_gap_conn_find(e.conn, &desc) == 0) {
                // The identity address, not the one on the air, which a
                // phone rotates; only bonding resolves one to the other.
                e.peer.type = desc.peer_id_addr.type;
                std::memcpy(e.peer.addr, desc.peer_id_addr.val, sizeof(e.peer.addr));
                if (e.code == 0 && !desc.sec_state.encrypted) e.code = -1;
            } else if (e.code == 0) {
                e.code = -1;
            }
            post(e);
            return 0;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (ev->subscribe.attr_handle == g_ctx.ctrl_val_handle) {
                e.kind = Event::Subscribe;
                e.flag = ev->subscribe.cur_notify != 0;
                post(e);
            }
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The phone forgot the bond and wants to pair again. Drop ours
            // and let it, rather than locking out the one phone this is for.
            ble_gap_conn_desc desc;
            if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            return 0;
    }
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &g_ctx.own_addr_type) != 0) {
        ESP_LOGE(kTag, "no usable address");
        return;
    }
    Event e{};
    e.kind = Event::Synced;
    post(e);
    start_advertising();
}

// NimBLE asks this when the bond store is full. Evicting the oldest bond
// is only right when a new phone is actually welcome; otherwise a stranger
// pairing three times would push the rider's own phone out. Refusing makes
// that pairing fail, which is the point.
int store_status(ble_store_status_event* ev, void* arg) {
    if (ev->event_code == BLE_STORE_EVENT_OVERFLOW && !g_ctx.may_evict.load()) {
        return BLE_HS_ESTORE_CAP;
    }
    return ble_store_util_status_rr(ev, arg);
}

void on_reset(int reason) {
    Event e{};
    e.kind = Event::Reset;
    e.code = reason;
    post(e);
}

void host_task(void*) {
    nimble_port_run();              // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// --- this task's side ----------------------------------------------------

void demote_hosted_tasks() {
    int found = 0, lowered = 0;
    for (const char* name : kHostedTasks) {
        if (TaskHandle_t t = xTaskGetHandle(name)) {
            ++found;
            if (uxTaskPriorityGet(t) > kHostedPrio) {
                vTaskPrioritySet(t, kHostedPrio);
                ++lowered;
            }
        }
    }
    // The bring-up check that this is still doing its job: a newer
    // esp_hosted that renames its tasks shows here as "0 of 0".
    ESP_LOGI(kTag, "esp_hosted tasks: %d found, %d lowered to %u",
             found, lowered, static_cast<unsigned>(kHostedPrio));
}

// The radio, from the SDIO link up. Every failure is logged, reported and
// returned from; the dash carries on without Bluetooth (§11.8).
bool bring_up(Deps& d, char* c6_fw, size_t c6_fw_len) {
    using core::state::Dev;
    d.health->begin_init(Dev::BleHost, now_ms());

    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(kTag, "no SDIO link to the C6");
        d.health->report_error(Dev::BleHost, 1, now_ms());
        return false;
    }
    esp_hosted_coprocessor_fwver_t v;
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        ESP_LOGI(kTag, "C6 firmware %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 v.major1, v.minor1, v.patch1);
        if (v.build > 0) {
            snprintf(c6_fw, c6_fw_len, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 " #%08" PRIx32,
                     v.major1, v.minor1, v.patch1, static_cast<uint32_t>(v.build));
        } else {
            snprintf(c6_fw, c6_fw_len, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 " #-",
                     v.major1, v.minor1, v.patch1);
        }
    }
    c6_firmware_update_if_needed();   // restarts the dash if it upgraded

    if (esp_hosted_bt_controller_init() != ESP_OK ||
        esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGE(kTag, "C6 BT controller did not start");
        d.health->report_error(Dev::BleHost, 2, now_ms());
        return false;
    }
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init: %s", esp_err_to_name(err));
        d.health->report_error(Dev::BleHost, static_cast<int32_t>(err), now_ms());
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = store_status;
    // §11.3: bond, and keep it in NVS so the phone reconnects every ride
    // without pairing again. Just Works: the dash has no way to show or
    // enter a passkey worth the trouble. The bond is not there to protect
    // song titles; it is how the dash tells the rider's phone from any other
    // (core/ble/pairing.hpp). nRF Connect can still poke Feed: it bonds.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(kSvcs) != 0 || ble_gatts_add_svcs(kSvcs) != 0) {
        ESP_LOGE(kTag, "GATT registration failed");
        d.health->report_error(Dev::BleHost, 4, now_ms());
        return false;
    }
    ble_svc_gap_device_name_set(kDeviceName);
    ble_store_config_init();

    // Pinned to core 1 by CONFIG_BT_NIMBLE_PINNED_TO_CORE_1; main.cpp
    // fails the build if that ever comes undone.
    nimble_port_freertos_init(host_task);
    return true;
}

void send_ctrl(uint16_t conn, uint8_t cmd) {
    uint8_t line[3];
    const size_t n = core::media::ctrl_line(cmd, line);
    if (n == 0) return;
    os_mbuf* om = ble_hs_mbuf_from_flat(line, n);
    if (!om) return;
    if (ble_gatts_notify_custom(conn, g_ctx.ctrl_val_handle, om) != 0) {
        ESP_LOGW(kTag, "ctrl notify failed");
    }
}

ble_addr_t to_ble_addr(const core::ble::PeerId& p) {
    ble_addr_t a{};
    a.type = p.type;
    std::memcpy(a.val, p.addr, sizeof(a.val));
    return a;
}

// The bonded peers, read back from NimBLE's store. Called whenever this task
// has changed the store, and at sync, so the cache cannot drift from it for
// long — NimBLE's own eviction and repeat-pairing also write the store.
void load_peers(core::ble::Peers& peers) {
    peers.clear();
    ble_addr_t addrs[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    if (ble_store_util_bonded_peers(addrs, &n, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) return;
    for (int i = 0; i < n; ++i) {
        core::ble::PeerId p;
        p.type = addrs[i].type;
        std::memcpy(p.addr, addrs[i].val, sizeof(p.addr));
        peers.add(p);
    }
}

void drop(uint16_t conn, uint8_t reason) {
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    const int rc = ble_gap_terminate(conn, reason);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(kTag, "terminate: %d", rc);
}

void ble_task(void* arg) {
    using core::state::Dev;
    Deps& d = *static_cast<Deps*>(arg);

    demote_hosted_tasks();
    core::state::BleDiag diag;
    if (!bring_up(d, diag.c6_fw, sizeof diag.c6_fw)) {
        d.slots->ble_diag.publish(diag);   // whatever was learned before it failed
        // Nothing to retry into: a C6 that will not answer at boot is a
        // module problem, not a loose connector. DIAG shows the fault.
        vTaskDelete(nullptr);
        return;
    }
    // The RPC tasks only exist once the link is up.
    demote_hosted_tasks();

    core::state::Media media;
    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    bool subscribed = false;
    bool synced = false;
    uint32_t dropped_at_disconnect_ms = 0;   // 0: nothing to clear
    uint32_t ctrl_gen = d.slots->media_ctrl.generation();
    uint32_t request_gen = d.slots->ble_request.generation();

    // Who may connect (§11.3). The policy is core::ble; this only acts on it.
    core::ble::Peers peers;
    core::ble::PairingWindow window;
    bool     encrypted = false;
    bool     dropping = false;      // terminate sent, waiting for Disconnect
    uint32_t connected_ms = 0;
    uint32_t last_diag_ms = 0;
    uint32_t dropped_seen = 0;

    d.slots->media.publish(media);
    d.slots->ble_diag.publish(diag);

    for (;;) {
        Event e;
        bool media_dirty = false;
        bool diag_dirty = false;
        TickType_t wait = pdMS_TO_TICKS(kIdleWaitMs);
        while (xQueueReceive(g_ctx.q, &e, wait) == pdTRUE) {
            wait = 0;   // drain whatever else arrived, then get on with it
            const uint32_t now = now_ms();
            switch (e.kind) {
                case Event::Line:
                    if (const char tag = core::media::apply_line(media, e.data, e.len, now)) {
                        diag.last_tag = tag;
                        media_dirty = diag_dirty = true;
                        d.health->report_ok(Dev::Phone, now);
                    }
                    break;
                case Event::Connect:
                    ESP_LOGI(kTag, "phone connected, waiting for encryption");
                    conn = e.conn;
                    connected_ms = now;
                    encrypted = false;
                    dropping = false;
                    dropped_at_disconnect_ms = 0;
                    d.health->begin_init(Dev::Phone, now);
                    d.health->report_ok(Dev::Phone, now);
                    break;
                case Event::Disconnect:
                    ESP_LOGI(kTag, "phone gone, reason %" PRId32, e.code);
                    conn = BLE_HS_CONN_HANDLE_NONE;
                    subscribed = false;
                    encrypted = false;
                    dropping = false;
                    diag.ctrl_subscribed = false;
                    diag.encrypted = false;
                    diag.has_rssi = false;
                    diag_dirty = true;
                    // Out of range is normal, not a fault.
                    d.health->set_absent(Dev::Phone);
                    dropped_at_disconnect_ms = now != 0 ? now : 1;
                    break;
                case Event::Subscribe:
                    subscribed = e.flag;
                    diag.ctrl_subscribed = e.flag;
                    diag_dirty = true;
                    break;
                case Event::Encrypted: {
                    if (e.conn != conn) break;
                    if (e.code != 0) {
                        // Usually a phone holding a bond this dash has
                        // forgotten: it must forget the dash too.
                        ESP_LOGW(kTag, "encryption failed (%" PRId32 "), dropping", e.code);
                        drop(conn, BLE_ERR_AUTH_FAIL);
                        dropping = true;
                        break;
                    }
                    const bool known = peers.contains(e.peer);
                    const bool open = window.is_open(now);
                    switch (core::ble::judge(known, peers.size(), open)) {
                        case core::ble::Verdict::Keep:
                            encrypted = true;
                            if (!known) {
                                ESP_LOGI(kTag, "new phone bonded");
                                window.close();   // one phone per window
                                load_peers(peers);
                            } else {
                                ESP_LOGI(kTag, "bonded phone back");
                            }
                            break;
                        case core::ble::Verdict::Reject: {
                            ESP_LOGW(kTag, "unknown phone outside pairing mode, refused");
                            const ble_addr_t a = to_ble_addr(e.peer);
                            ble_store_util_delete_peer(&a);
                            load_peers(peers);
                            drop(conn, BLE_ERR_AUTH_FAIL);
                            dropping = true;
                            break;
                        }
                        case core::ble::Verdict::Defer:
                            ESP_LOGI(kTag, "bonded phone held off while pairing mode is open");
                            drop(conn, BLE_ERR_REM_USER_CONN_TERM);
                            dropping = true;
                            break;
                    }
                    diag.encrypted = encrypted;
                    diag_dirty = true;
                    break;
                }
                case Event::Synced:
                    ESP_LOGI(kTag, "advertising as \"%s\"", kDeviceName);
                    synced = true;
                    // The store is loaded from NVS by now.
                    load_peers(peers);
                    ESP_LOGI(kTag, "%u bonded phone(s)", static_cast<unsigned>(peers.size()));
                    d.health->report_ok(Dev::BleHost, now);
                    break;
                case Event::Reset:
                    // NimBLE resyncs with the controller on its own and
                    // sends Synced again when it has.
                    ESP_LOGW(kTag, "host reset, reason %" PRId32, e.code);
                    synced = false;
                    conn = BLE_HS_CONN_HANDLE_NONE;
                    subscribed = false;
                    encrypted = false;
                    dropping = false;
                    diag.ctrl_subscribed = false;
                    diag.encrypted = false;
                    d.health->report_error(Dev::BleHost, e.code, now);
                    d.health->set_absent(Dev::Phone);
                    dropped_at_disconnect_ms = now != 0 ? now : 1;
                    break;
            }
        }

        const uint32_t now = now_ms();

        // §11.8: hold the last song briefly through a dropout, then clear
        // it. A link that blinks and comes back keeps its title.
        if (dropped_at_disconnect_ms != 0 &&
            now - dropped_at_disconnect_ms >= core::media::kDisconnectHoldMs) {
            dropped_at_disconnect_ms = 0;
            media = core::state::Media{};
            media_dirty = true;
        }
        if (media_dirty) d.slots->media.publish(media);

        // CONFIG's phone actions, posted by `logic`.
        const uint32_t req_gen = d.slots->ble_request.generation();
        if (req_gen != request_gen) {
            request_gen = req_gen;
            switch (d.slots->ble_request.get().op) {
                case core::state::BleRequest::ForgetPhone:
                    // Every bond, and the link: the next phone pairs freely.
                    // The phone keeps its half of the bond until the rider
                    // forgets the dash in Android's Bluetooth settings too.
                    ESP_LOGI(kTag, "forgetting every bonded phone");
                    window.close();
                    if (ble_store_clear() != 0) ESP_LOGW(kTag, "bond store clear failed");
                    load_peers(peers);
                    if (conn != BLE_HS_CONN_HANDLE_NONE && !dropping) {
                        drop(conn, BLE_ERR_REM_USER_CONN_TERM);
                        dropping = true;
                    }
                    break;
                case core::state::BleRequest::PairingMode:
                    // The one connection has to be free for the new phone to
                    // find the dash, so the current one is let go; it is
                    // held off by Defer until the window closes.
                    ESP_LOGI(kTag, "pairing mode for %u s",
                             static_cast<unsigned>(core::ble::kPairingWindowMs / 1000));
                    window.open(now);
                    if (conn != BLE_HS_CONN_HANDLE_NONE && !dropping) {
                        drop(conn, BLE_ERR_REM_USER_CONN_TERM);
                        dropping = true;
                    }
                    break;
                default:
                    break;
            }
        }

        // A link that has not encrypted in time is not the rider's phone, or
        // is a phone whose pairing prompt nobody answered. Either way it is
        // holding the only connection.
        if (conn != BLE_HS_CONN_HANDLE_NONE && !dropping &&
            core::ble::encrypt_overdue(connected_ms, now, encrypted)) {
            ESP_LOGW(kTag, "phone never encrypted, dropping");
            drop(conn, BLE_ERR_AUTH_FAIL);
            dropping = true;
        }

        const bool open = window.is_open(now);
        g_ctx.may_evict.store(core::ble::may_evict(peers.size(), open));
        const uint8_t pairing_s = static_cast<uint8_t>(window.remaining_s(now));
        const uint8_t bonds = static_cast<uint8_t>(peers.size());
        if (pairing_s != diag.pairing_s || bonds != diag.bonds) {
            diag.pairing_s = pairing_s;
            diag.bonds = bonds;
            diag_dirty = true;
        }

        // A handlebar transport press, posted by `logic`. The generation
        // says whether it is new; a press with no phone listening is
        // dropped, not queued for a phone that may never come.
        const uint32_t gen = d.slots->media_ctrl.generation();
        if (gen != ctrl_gen) {
            ctrl_gen = gen;
            if (conn != BLE_HS_CONN_HANDLE_NONE && subscribed && encrypted) {
                send_ctrl(conn, d.slots->media_ctrl.get().cmd);
            }
        }

        if (now - last_diag_ms >= kDiagPeriodMs) {
            last_diag_ms = now;
            if (conn != BLE_HS_CONN_HANDLE_NONE) {
                int8_t rssi = 0;
                if (ble_gap_conn_rssi(conn, &rssi) == 0) {
                    diag.rssi_dbm = rssi;
                    diag.has_rssi = true;
                }
                d.health->report_ok(Dev::Phone, now);
            }
            if (synced) d.health->report_ok(Dev::BleHost, now);
            const uint32_t dropped = g_ctx.dropped.load();
            if (dropped != dropped_seen) {
                ESP_LOGW(kTag, "%" PRIu32 " events lost to a full queue", dropped - dropped_seen);
                dropped_seen = dropped;
            }
            diag_dirty = true;
        }
        if (diag_dirty) d.slots->ble_diag.publish(diag);
    }
}

}  // namespace

void start_ble(Deps& d) {
    g_ctx.q = xQueueCreate(16, sizeof(Event));
    if (!g_ctx.q) {
        d.health->report_error(core::state::Dev::BleHost, -1, d.clock->now_ms());
        return;
    }
    xTaskCreatePinnedToCore(ble_task, "ble", 4096, &d, 4, nullptr, 1);
}

}  // namespace services
