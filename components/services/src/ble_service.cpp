// `ble` — core 1, priority 4, event driven (§11.4).
//
// The P4 has no radio. The BLE controller runs on the module's ESP32-C6 and
// is reached over SDIO through ESP-Hosted; NimBLE, the host stack, runs here
// on core 1. `demo_p4/main/ble_media.cpp` is where this first worked, and
// this is that code brought under the five rules.
//
// What this task owns: the Dash Media GATT service (§11.1), and the `media`,
// `notify`, `heart` and `ble_diag` slots, which nothing else writes. NimBLE's callbacks run on
// its own host task, so they do no work there: each one copies what
// happened into a queue and returns (§11.8), and this task drains the queue,
// runs `core::media` over the lines and publishes. That keeps the one-writer
// rule literal rather than "one writer, most of the time".
//
// An iPhone needs no app (§11.1.1): once it is let in, the dash looks
// through its services as a GATT client, and if AMS and ANCS are there it
// subscribes to them (AppleLink, core/media/apple). An Android phone has
// neither and writes Feed instead; the same slots are filled either way.
//
// The dash is also a central, for one thing: the rider's watch's heart rate
// over the standard Heart Rate Service (core/hr), when CONFIG -> Heart rate
// -> Watch HR is on. It scans, connects to the sensor core::hr::Picker
// chooses, discovers the measurement characteristic and subscribes. The
// watch's connection has its own GAP callback, so the phone's handling never
// sees it, and its bond (if it asks for one) is never stored: the store holds
// phones, and core::ble counts them.
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
#include "core/hr/hr.hpp"
#include "core/media/apple.hpp"
#include "core/media/feed.hpp"

#include "esp_hosted.h"
#include "esp_hosted_event.h"
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

// --- NimBLE's FreeRTOS callouts, mended ------------------------------------
//
// IDF v6.0.2's NimBLE port, with CONFIG_BT_NIMBLE_USE_ESP_TIMER=n (which this
// dash needs, see sdkconfig.defaults), never stores a callout's event queue
// when it first creates it: npl_freertos_callout_init sets `evq` only when
// asked to init a callout that already exists. So every NimBLE timer ran its
// handler on FreeRTOS's timer task instead of queueing it for the host task.
// Peripheral-only, that went unseen. With the central role on (the watch,
// core/hr), ble_gap_init arms the host timer at once; it fired on the timer
// task while the host was still syncing with the C6 and asserted
// (ble_hs_timer_exp, ble_hs.c:570), a boot loop on the demo board.
//
// The mend: wrap the function that fills NimBLE's porting table (the linker's
// --wrap, services/CMakeLists.txt and demo_p4/main/CMakeLists.txt) and swap in
// a callout init that calls the original twice. The second call takes the
// branch that stores the queue. If an IDF upgrade fixes the port, the second
// call does nothing new.
extern "C" {
void __real_npl_freertos_funcs_init(void);
struct npl_funcs_t* npl_freertos_funcs_get(void);
}

namespace {
// Rule 1's other exception here, with g_ctx: the porting table holds plain
// function pointers, so the original has to be kept somewhere they can see.
int (*g_real_callout_init)(ble_npl_callout*, ble_npl_eventq*, ble_npl_event_fn*, void*) = nullptr;

int callout_init_with_queue(ble_npl_callout* co, ble_npl_eventq* evq,
                            ble_npl_event_fn* fn, void* arg) {
    const int rc = g_real_callout_init(co, evq, fn, arg);
    if (rc != 0) return rc;
    return g_real_callout_init(co, evq, fn, arg);
}
}  // namespace

extern "C" void __wrap_npl_freertos_funcs_init(void) {
    __real_npl_freertos_funcs_init();
    npl_funcs_t* f = npl_freertos_funcs_get();
    if (f && f->p_ble_npl_callout_init != callout_init_with_queue) {
        g_real_callout_init = f->p_ble_npl_callout_init;
        f->p_ble_npl_callout_init = callout_init_with_queue;
    }
}

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
//
// "sys_evt" is IDF's default event loop, which ESP-Hosted and `wifi` both
// need. IDF pins it to core 0 at priority 20, above ctrl, and every Wi-Fi
// and netif handler runs on it; it cannot be moved, so it is lowered too.
constexpr UBaseType_t kHostedPrio = 8;
constexpr const char* kHostedTasks[] = {
    "sdio_rx_buf", "sdio_read", "sdio_process_rx", "sdio_write",
    "rpc_rx", "rpc_tx", "rpc_supp_cb", "sys_evt",
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

// An iPhone's AMS and ANCS (core/media/apple), which keeps the bytes so a
// host test can check them against Apple's strings.
ble_uuid128_t uuid128(const uint8_t (&le)[16]) {
    ble_uuid128_t u{};
    u.u.type = BLE_UUID_TYPE_128;
    std::memcpy(u.value, le, sizeof u.value);
    return u;
}
const ble_uuid128_t kAmsUuid = uuid128(core::media::ams::kService);
const ble_uuid128_t kAmsRemoteUuid = uuid128(core::media::ams::kRemoteCommand);
const ble_uuid128_t kAmsEntityUuid = uuid128(core::media::ams::kEntityUpdate);
const ble_uuid128_t kAncsUuid = uuid128(core::media::ancs::kService);
const ble_uuid128_t kAncsSourceUuid = uuid128(core::media::ancs::kNotificationSource);
const ble_uuid128_t kAncsControlUuid = uuid128(core::media::ancs::kControlPoint);
const ble_uuid128_t kAncsDataUuid = uuid128(core::media::ancs::kDataSource);

// Which of those a discovery callback found, in Event::which.
enum AppleId : uint8_t {
    kAppleNone = 0, kAmsSvc, kAncsSvc,
    kAmsRemote, kAmsEntity, kAncsSource, kAncsControl, kAncsData,
};

// The scan response's shortened name. ANCS's solicitation takes 18 of its 31
// bytes, so "CRF250F-Dash" no longer fits beside it; a phone reads the whole
// name from the GAP service once connected.
constexpr char kShortName[] = "CRF250F";

// The watch's (core/hr). BLE_UUID16_DECLARE is a C compound literal.
const ble_uuid16_t kHrsUuid = BLE_UUID16_INIT(core::hr::kServiceUuid);
const ble_uuid16_t kHrmUuid = BLE_UUID16_INIT(core::hr::kMeasurementUuid);

// What a NimBLE callback hands to this task.
struct Event {
    enum Kind : uint8_t {
        Line, Connect, Disconnect, Subscribe, Synced, Reset, Encrypted,
        // ESP-Hosted gave up on the SDIO link: the C6 stopped answering.
        RadioLost,
        // The watch (core/hr), each step of finding and subscribing to it.
        HrAdv, HrScanDone, HrConnect, HrConnectFail, HrDisconnect,
        HrSvc, HrChr, HrDsc, HrSubscribed, HrEnc, HrData,
        // The iPhone's AMS and ANCS (AppleLink).
        AppleSvc, AppleChr, AppleDsc, AppleWrite, AppleData,
    };
    Kind     kind;
    uint8_t  len;       // Line: bytes in `data`
    bool     flag;      // Subscribe: notifications on
    uint16_t conn;      // Connect: the handle
    int32_t  code;      // Disconnect / Reset: NimBLE's reason; Encrypted: status
    core::ble::PeerId peer;   // Encrypted: the peer's identity address; HrAdv: the advertiser
    uint16_t h1 = 0, h2 = 0;  // Hr*, Apple*: attribute handles (a service's start and end)
    uint8_t  which = kAppleNone;   // Apple*: what was found
    // The longest is a whole ANCS answer (137 bytes); then a notification
    // line (§11.1 `N`, 130); a media field is 61. Anything past this would
    // be cut by core::media anyway, and an ANCS answer is never longer (it
    // is asked for in bounded lengths). Sixteen of these are the queue:
    // 2.4 kB of internal RAM, 1.2 more than when only media came this way.
    uint8_t  data[core::media::ancs::kMaxResponse > core::media::kMaxNotifyLine
                      ? core::media::ancs::kMaxResponse
                      : core::media::kMaxNotifyLine];
};

struct Ctx {
    QueueHandle_t q = nullptr;
    uint8_t  own_addr_type = 0;
    uint16_t ctrl_val_handle = 0;     // filled in by NimBLE at registration
    std::atomic<uint32_t> dropped{0}; // events lost to a full queue
    // Phone links up, counted on the host task by the phone's GAP callback
    // as they connect and drop. The `ble` task's own `conn` lags the host by
    // a queue hop; this does not, so re-arming advertising on it can never
    // let a second phone in behind the first.
    std::atomic<int> phone_links{0};
    // Written by the `ble` task, read by store_status() on the host task:
    // whether a new bond may push the oldest out of a full store.
    std::atomic<bool> may_evict{true};
    // Written by the `ble` task, read by the store gate on the host task
    // (core::ble::may_store): whether the bike is still enough to write
    // flash, and whether a new phone may bond.
    std::atomic<bool> still{false};
    std::atomic<bool> welcome{false};
    // A bonded phone that asked to pair again (REPEAT_PAIRING): its old
    // bond is deleted first, and its new one must not then be refused as a
    // stranger's. Host task only, like the store calls that read it.
    ble_addr_t rebond{};
    bool       rebond_valid = false;
    // NimBLE's own NVS-backed store, which the gate wraps.
    int (*store_read)(int, const ble_store_key*, ble_store_value*) = nullptr;
    int (*store_write)(int, const ble_store_value*) = nullptr;
    int (*store_delete)(int, const ble_store_key*) = nullptr;

    // The watch. Set by the `ble` task before it connects, and only then,
    // so the host task's reads cannot see one half written: the store gate
    // refuses its bond, and NOTIFY_RX passes on only its measurements.
    ble_addr_t hr_peer{};
    std::atomic<bool>     hr_peer_valid{false};
    std::atomic<uint16_t> hr_conn{BLE_HS_CONN_HANDLE_NONE};
    std::atomic<uint16_t> hr_val{0};
    // Every advertising report a watch scan received, anyone's: proof the
    // scan is alive when no heart-rate advertiser is among them.
    std::atomic<uint32_t> scan_reports{0};
    // Advertisers heard with the Heart Rate Service this scan, so each is
    // passed on once, and once more with its name (usually in the scan
    // response), and nobody else is. The controller's duplicate filter is
    // off (see `scan`), so this is the only one. Host task only, cleared by
    // the `ble` task before it starts a scan.
    static constexpr int kHrSeen = 8;
    ble_addr_t hr_seen[kHrSeen] = {};
    bool       hr_named[kHrSeen] = {};
    int        hr_seen_n = 0;
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

    // The scan response, by hand: NimBLE's field builder has no service
    // solicitation. Soliciting ANCS is what makes an iPhone list the dash
    // under Settings -> Bluetooth, and serve it AMS and ANCS once bonded
    // (§11.1.1). Android ignores it.
    static_assert(2 + 16 + 2 + sizeof kShortName - 1 <= BLE_HS_ADV_MAX_SZ,
                  "scan response over 31 bytes");
    uint8_t rsp[BLE_HS_ADV_MAX_SZ];
    size_t k = 0;
    rsp[k++] = 1 + 16;
    rsp[k++] = 0x15;   // list of 128-bit service solicitation UUIDs
    std::memcpy(rsp + k, core::media::ancs::kService, 16);
    k += 16;
    rsp[k++] = 1 + sizeof kShortName - 1;
    rsp[k++] = 0x08;   // shortened local name
    std::memcpy(rsp + k, kShortName, sizeof kShortName - 1);
    k += sizeof kShortName - 1;
    if (ble_gap_adv_rsp_set_data(rsp, static_cast<int>(k)) != 0) {
        ESP_LOGE(kTag, "scan response rejected");
        return;
    }

    ble_gap_adv_params p{};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(g_ctx.own_addr_type, nullptr, BLE_HS_FOREVER,
                                     &p, gap_event, nullptr);
    // Already on is fine (the loop and a callback can both ask). Anything
    // else is retried by the loop within a second (keep_advertising).
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(kTag, "advertising failed: %d", rc);
}

int gap_event(ble_gap_event* ev, void*) {
    Event e{};
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status != 0) {
                start_advertising();
                return 0;
            }
            ++g_ctx.phone_links;
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
            if (g_ctx.phone_links.load() > 0) --g_ctx.phone_links;
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
        case BLE_GAP_EVENT_NOTIFY_RX: {
            // Only an iPhone notifies the dash: AMS and ANCS (AppleLink).
            e.kind = Event::AppleData;
            e.conn = ev->notify_rx.conn_handle;
            e.h1 = ev->notify_rx.attr_handle;
            uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
            if (len > sizeof(e.data)) len = sizeof(e.data);
            if (ble_hs_mbuf_to_flat(ev->notify_rx.om, e.data, len, nullptr) != 0) return 0;
            e.len = static_cast<uint8_t>(len);
            post(e);   // the stack frees the mbuf
            return 0;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The phone forgot the bond and wants to pair again. Drop ours
            // and let it, rather than locking out the one phone this is for.
            // Not while moving: the old bond could be deleted but the new
            // one not written, and the phone would be locked out instead.
            // It asks again on its next connection.
            if (!g_ctx.still.load()) return BLE_GAP_REPEAT_PAIRING_IGNORE;
            ble_gap_conn_desc desc;
            if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
                g_ctx.rebond = desc.peer_id_addr;
                g_ctx.rebond_valid = true;
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            return 0;
    }
}

// --- the watch, NimBLE side: same rule, copy and return -------------------

void hr_post(Event::Kind k, uint16_t conn, int32_t code, uint16_t h1 = 0, uint16_t h2 = 0) {
    Event e{};
    e.kind = k;
    e.conn = conn;
    e.code = code;
    e.h1 = h1;
    e.h2 = h2;
    post(e);
}

int hr_disc_event(ble_gap_event* ev, void*) {
    if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        hr_post(Event::HrScanDone, 0, ev->disc_complete.reason);
        return 0;
    }
    if (ev->type != BLE_GAP_EVENT_DISC) return 0;
    g_ctx.scan_reports.fetch_add(1, std::memory_order_relaxed);
    const ble_gap_disc_desc& dd = ev->disc;
    const core::hr::Advert a = core::hr::parse_advert(dd.data, dd.length_data);
    int at = -1;
    for (int i = 0; i < g_ctx.hr_seen_n && at < 0; ++i) {
        if (ble_addr_cmp(&g_ctx.hr_seen[i], &dd.addr) == 0) at = i;
    }
    bool pass = false;
    if (at < 0) {
        // New this scan: only a Heart Rate advertiser, and only while there
        // is room to remember it, or every advert it sends would pass.
        if (!a.hrs || g_ctx.hr_seen_n >= Ctx::kHrSeen) return 0;
        at = g_ctx.hr_seen_n++;
        g_ctx.hr_seen[at] = dd.addr;
        g_ctx.hr_named[at] = a.name[0] != '\0';
        pass = true;
    } else if (a.name[0] && !g_ctx.hr_named[at]) {
        // Known, and now its name (the scan response, as a rule).
        g_ctx.hr_named[at] = true;
        pass = true;
    }
    if (!pass) return 0;
    Event e{};
    e.kind = Event::HrAdv;
    e.code = dd.rssi;
    e.peer.type = dd.addr.type;
    std::memcpy(e.peer.addr, dd.addr.val, sizeof(e.peer.addr));
    const size_t n = std::strlen(a.name);
    std::memcpy(e.data, a.name, n);
    e.len = static_cast<uint8_t>(n);
    post(e);
    return 0;
}

int hr_conn_event(ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            hr_post(ev->connect.status == 0 ? Event::HrConnect : Event::HrConnectFail,
                    ev->connect.conn_handle, ev->connect.status);
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            hr_post(Event::HrDisconnect, ev->disconnect.conn.conn_handle, ev->disconnect.reason);
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            hr_post(Event::HrEnc, ev->enc_change.conn_handle, ev->enc_change.status);
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            if (ev->notify_rx.conn_handle != g_ctx.hr_conn.load() ||
                ev->notify_rx.attr_handle != g_ctx.hr_val.load()) {
                return 0;
            }
            Event e{};
            e.kind = Event::HrData;
            e.conn = ev->notify_rx.conn_handle;
            uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
            if (len > sizeof(e.data)) len = sizeof(e.data);
            if (ble_hs_mbuf_to_flat(ev->notify_rx.om, e.data, len, nullptr) != 0) return 0;
            e.len = static_cast<uint8_t>(len);
            post(e);   // the stack frees the mbuf
            return 0;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            // The watch remembers a bond this dash never kept. Not deleted:
            // there is nothing of ours to delete, and nothing is written.
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        default:
            return 0;
    }
}

int hr_svc_cb(uint16_t conn, const ble_gatt_error* err, const ble_gatt_svc* svc, void*) {
    if (err->status == 0 && svc) {
        hr_post(Event::HrSvc, conn, 0, svc->start_handle, svc->end_handle);
    } else {
        hr_post(Event::HrSvc, conn, err->status);   // BLE_HS_EDONE: the end
    }
    return 0;
}

int hr_chr_cb(uint16_t conn, const ble_gatt_error* err, const ble_gatt_chr* chr, void*) {
    if (err->status == 0 && chr) {
        hr_post(Event::HrChr, conn, 0, chr->val_handle, chr->def_handle);
    } else {
        hr_post(Event::HrChr, conn, err->status);
    }
    return 0;
}

int hr_dsc_cb(uint16_t conn, const ble_gatt_error* err, uint16_t, const ble_gatt_dsc* dsc, void*) {
    if (err->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == core::hr::kCccdUuid) {
            hr_post(Event::HrDsc, conn, 0, dsc->handle);
        }
    } else {
        hr_post(Event::HrDsc, conn, err->status);
    }
    return 0;
}

int hr_write_cb(uint16_t conn, const ble_gatt_error* err, ble_gatt_attr*, void*) {
    hr_post(Event::HrSubscribed, conn, err->status);
    return 0;
}

// --- the iPhone, NimBLE side: same rule, copy and return -------------------

void apple_post(Event::Kind k, uint16_t conn, int32_t code, uint8_t which = kAppleNone,
                uint16_t h1 = 0, uint16_t h2 = 0) {
    Event e{};
    e.kind = k;
    e.conn = conn;
    e.code = code;
    e.which = which;
    e.h1 = h1;
    e.h2 = h2;
    post(e);
}

int apple_svc_cb(uint16_t conn, const ble_gatt_error* err, const ble_gatt_svc* svc, void*) {
    if (err->status != 0 || !svc) {
        apple_post(Event::AppleSvc, conn, err->status);   // BLE_HS_EDONE: the end
        return 0;
    }
    const uint8_t which = ble_uuid_cmp(&svc->uuid.u, &kAmsUuid.u) == 0    ? kAmsSvc
                          : ble_uuid_cmp(&svc->uuid.u, &kAncsUuid.u) == 0 ? kAncsSvc
                                                                           : kAppleNone;
    if (which != kAppleNone) {
        apple_post(Event::AppleSvc, conn, 0, which, svc->start_handle, svc->end_handle);
    }
    return 0;
}

int apple_chr_cb(uint16_t conn, const ble_gatt_error* err, const ble_gatt_chr* chr, void*) {
    if (err->status != 0 || !chr) {
        apple_post(Event::AppleChr, conn, err->status);
        return 0;
    }
    struct { const ble_uuid128_t* uuid; uint8_t id; } const kKnown[] = {
        {&kAmsRemoteUuid, kAmsRemote},   {&kAmsEntityUuid, kAmsEntity},
        {&kAncsSourceUuid, kAncsSource}, {&kAncsControlUuid, kAncsControl},
        {&kAncsDataUuid, kAncsData},
    };
    for (const auto& k : kKnown) {
        if (ble_uuid_cmp(&chr->uuid.u, &k.uuid->u) == 0) {
            apple_post(Event::AppleChr, conn, 0, k.id, chr->val_handle);
            break;
        }
    }
    return 0;
}

int apple_dsc_cb(uint16_t conn, const ble_gatt_error* err, uint16_t, const ble_gatt_dsc* dsc, void*) {
    if (err->status != 0 || !dsc) {
        apple_post(Event::AppleDsc, conn, err->status);
    } else if (ble_uuid_u16(&dsc->uuid.u) == core::hr::kCccdUuid) {
        apple_post(Event::AppleDsc, conn, 0, kAppleNone, dsc->handle);
    }
    return 0;
}

int apple_write_cb(uint16_t conn, const ble_gatt_error* err, ble_gatt_attr*, void*) {
    apple_post(Event::AppleWrite, conn, err->status);
    return 0;
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

// The bond store, gated (core::ble::may_store). NimBLE calls these with its
// host lock held, on its host task, for every record it would persist; the
// config store writes each one to NVS at once, and an NVS write freezes
// core 0. Refusing is safe: NimBLE logs a failed store write and carries
// on, and the link stays up and encrypted for this connection either way.

// Whether `addr` has no bond in the store. Read straight from the config
// store's RAM table: the host lock is already held here.
bool unbonded(const ble_addr_t& addr) {
    if (g_ctx.rebond_valid && ble_addr_cmp(&addr, &g_ctx.rebond) == 0) return false;
    ble_store_key k{};
    k.sec.peer_addr = addr;
    k.sec.idx = 0;
    ble_store_value v;
    return g_ctx.store_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &k, &v) != 0;
}

// Whether `addr` is the watch's. Its bond is never stored: the store is the
// phones', and a watch in it would count as one (core::ble::Peers).
bool is_watch(const ble_addr_t& addr) {
    return g_ctx.hr_peer_valid.load() && ble_addr_cmp(&addr, &g_ctx.hr_peer) == 0;
}

int gated_store_write(int type, const ble_store_value* val) {
    switch (type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_SEC:
            if (is_watch(val->sec.peer_addr)) return BLE_HS_ESTORE_FAIL;
            break;
        case BLE_STORE_OBJ_TYPE_CCCD:
            if (is_watch(val->cccd.peer_addr)) return BLE_HS_ESTORE_FAIL;
            break;
        default: break;
    }
    bool stranger = false;
    switch (type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_SEC: stranger = unbonded(val->sec.peer_addr); break;
        case BLE_STORE_OBJ_TYPE_CCCD:     stranger = unbonded(val->cccd.peer_addr); break;
        case BLE_STORE_OBJ_TYPE_CSFC:     stranger = unbonded(val->csfc.peer_addr); break;
        case BLE_STORE_OBJ_TYPE_PEER_ADDR: stranger = unbonded(val->rpa_rec.peer_addr); break;
        default: break;
    }
    if (!core::ble::may_store(g_ctx.still.load(), stranger, g_ctx.welcome.load())) {
        return BLE_HS_ESTORE_FAIL;
    }
    const int rc = g_ctx.store_write(type, val);
    // The re-pairing phone's new bond is in: it is an ordinary peer again.
    if (rc == 0 && type == BLE_STORE_OBJ_TYPE_PEER_SEC && g_ctx.rebond_valid &&
        ble_addr_cmp(&val->sec.peer_addr, &g_ctx.rebond) == 0) {
        g_ctx.rebond_valid = false;
    }
    return rc;
}

int gated_store_delete(int type, const ble_store_key* key) {
    if (!core::ble::may_store(g_ctx.still.load(), false, true)) return BLE_HS_ESTORE_FAIL;
    return g_ctx.store_delete(type, key);
}

// ESP-Hosted's, on the default event loop's task. With
// CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE off it no longer restarts
// the dash when the C6 stops answering; this is how `ble` hears of it.
void on_transport_failure(void*, esp_event_base_t, int32_t, void*) {
    Event e{};
    e.kind = Event::RadioLost;
    post(e);
}

void on_reset(int reason) {
    g_ctx.phone_links.store(0);   // every link went with the controller
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

}  // namespace

// Lowers whichever of ESP-Hosted's tasks exist now. Called by main before
// `ctrl` starts (the constructor has already made the SDIO ones by then),
// by `ble` once the RPC tasks exist, by `wifi` after it starts Wi-Fi, and
// once a second from `ble`'s loop, because a C6 reset makes the transport
// and RPC tasks again, at 23. Quiet unless it lowered something, except
// when asked to report: a newer esp_hosted that renamed its tasks shows in
// that report as "0 found".
void demote_radio_tasks(bool report) {
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
    if (report || lowered) {
        ESP_LOGI(kTag, "esp_hosted tasks: %d found, %d lowered to %u",
                 found, lowered, static_cast<unsigned>(kHostedPrio));
    }
}

namespace {

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
    // ESP-Hosted posts its events to the default loop but makes it only on
    // a Wi-Fi path this firmware does not take: without it the failure was
    // posted into nothing. `wifi` makes it too; whichever is second gets
    // INVALID_STATE. Its task, sys_evt, is one demote_radio_tasks lowers.
    const esp_err_t loop = esp_event_loop_create_default();
    if ((loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) ||
        esp_event_handler_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
                                   on_transport_failure, nullptr) != ESP_OK) {
        ESP_LOGW(kTag, "no event loop: a lost C6 will not be reported");
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
    // Wrap the store ble_store_config_init just installed, so nothing it
    // persists reaches flash while the bike moves (gated_store_write).
    g_ctx.store_read = ble_hs_cfg.store_read_cb;
    g_ctx.store_write = ble_hs_cfg.store_write_cb;
    g_ctx.store_delete = ble_hs_cfg.store_delete_cb;
    ble_hs_cfg.store_write_cb = gated_store_write;
    ble_hs_cfg.store_delete_cb = gated_store_delete;

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

// The watch (core/hr), from this task's side: scan, choose, connect,
// discover, subscribe, and publish the `heart` slot. Every step is started
// here and its answer comes back through the queue; a step that never
// answers is given up on after kHrStepMs. Nothing here waits.
class HrLink {
public:
    explicit HrLink(Deps& d) : d_(d) {}

    void publish_initial() { d_.slots->heart.publish(heart_); }

    // Before each event: whether this task's state machine cares about it.
    void on_event(const Event& e, uint32_t now) {
        switch (e.kind) {
            case Event::HrAdv: {
                if (step_ != Step::Scanning) break;
                core::hr::Addr a;
                a.type = e.peer.type;
                std::memcpy(a.val, e.peer.addr, sizeof a.val);
                char name[sizeof heart_.name] = {};
                std::memcpy(name, e.data, e.len < sizeof name - 1 ? e.len : sizeof name - 1);
                picker_.heard(a, static_cast<int8_t>(e.code), name);
                ++heard_;
                break;
            }
            case Event::HrScanDone:
                if (step_ != Step::Scanning) break;
                ++scans_done_;
                if (e.code != 0) ESP_LOGW(kTag, "watch scan ended: %" PRId32, e.code);
                // Once per change, not every scan: enough to tell "the watch
                // is not broadcasting" from "the dash is not listening".
                if (heard_ != heard_logged_) {
                    heard_logged_ = heard_;
                    ESP_LOGI(kTag, "watch scan: %u heart-rate adverts heard", heard_);
                }
                connect_to_choice(now);
                break;
            case Event::HrConnect:
                if (step_ != Step::Connecting) {
                    // Turned off while connecting: not wanted after all.
                    ble_gap_terminate(e.conn, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                conn_ = e.conn;
                heart_.worn = false;   // worn on this link, not on the last one
                heart_.bpm = 0;
                g_ctx.hr_conn.store(conn_);
                picker_.latch(target_, heart_.name);
                ESP_LOGI(kTag, "watch connected (%s), finding heart rate", heart_.name);
                set_step(Step::Discovering, now);
                svc_start_ = svc_end_ = val_ = cccd_ = 0;
                secured_ = false;
                if (ble_gattc_disc_svc_by_uuid(conn_, &kHrsUuid.u,
                                               hr_svc_cb, nullptr) != 0) {
                    give_up("service discovery would not start", now);
                }
                break;
            case Event::HrConnectFail:
                if (step_ == Step::Connecting) {
                    ESP_LOGW(kTag, "watch connect failed: %" PRId32, e.code);
                    wait(now, kRetryMs);
                }
                break;
            case Event::HrDisconnect:
                if (e.conn != conn_) break;
                ESP_LOGI(kTag, "watch gone, reason %" PRId32, e.code);
                conn_ = BLE_HS_CONN_HANDLE_NONE;
                g_ctx.hr_conn.store(conn_);
                g_ctx.hr_val.store(0);
                heart_.rssi_dbm = 0;
                if (step_ != Step::Off) wait(now, kRetryMs);
                break;
            case Event::HrSvc:
                if (e.conn != conn_ || step_ != Step::Discovering) break;
                if (e.code == 0) {
                    svc_start_ = e.h1;
                    svc_end_ = e.h2;
                } else if (e.code == BLE_HS_EDONE && svc_start_ != 0) {
                    if (ble_gattc_disc_chrs_by_uuid(conn_, svc_start_, svc_end_,
                                                    &kHrmUuid.u,
                                                    hr_chr_cb, nullptr) != 0) {
                        give_up("characteristic discovery would not start", now);
                    }
                } else {
                    give_up("no Heart Rate service", now);
                }
                break;
            case Event::HrChr:
                if (e.conn != conn_ || step_ != Step::Discovering) break;
                if (e.code == 0) {
                    if (val_ == 0) val_ = e.h1;
                } else if (e.code == BLE_HS_EDONE && val_ != 0) {
                    g_ctx.hr_val.store(val_);
                    if (val_ >= svc_end_ ||
                        ble_gattc_disc_all_dscs(conn_, val_, svc_end_, hr_dsc_cb, nullptr) != 0) {
                        // No room for descriptors to search: the CCCD is
                        // conventionally the next handle.
                        cccd_ = static_cast<uint16_t>(val_ + 1);
                        subscribe(now);
                    }
                } else {
                    give_up("no Heart Rate Measurement", now);
                }
                break;
            case Event::HrDsc:
                if (e.conn != conn_ || step_ != Step::Discovering) break;
                if (e.code == 0) {
                    if (cccd_ == 0) cccd_ = e.h1;
                } else {
                    if (cccd_ == 0) cccd_ = static_cast<uint16_t>(val_ + 1);
                    subscribe(now);
                }
                break;
            case Event::HrSubscribed:
                if (e.conn != conn_ || step_ != Step::Discovering) break;
                if (e.code == 0) {
                    ESP_LOGI(kTag, "heart rate live from %s", heart_.name);
                    set_step(Step::Live, now);
                    last_data_ms_ = now;
                } else if (!secured_ &&
                           (e.code == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
                            e.code == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC))) {
                    // A sensor that wants an encrypted link. It pairs for this
                    // connection only; the store gate keeps its bond out.
                    secured_ = true;
                    ESP_LOGI(kTag, "watch wants encryption, pairing for this connection");
                    if (ble_gap_security_initiate(conn_) != 0) give_up("could not encrypt", now);
                } else {
                    ESP_LOGW(kTag, "subscribe refused: %" PRId32, e.code);
                    give_up("subscribe refused", now);
                }
                break;
            case Event::HrEnc:
                if (e.conn != conn_ || step_ != Step::Discovering) break;
                if (e.code == 0) subscribe(now);
                else give_up("encryption failed", now);
                break;
            case Event::HrData: {
                if (e.conn != conn_) break;
                core::hr::Measurement m;
                if (!core::hr::parse(e.data, e.len, m)) break;
                if (step_ == Step::Discovering) set_step(Step::Live, now);   // beat the write's answer
                last_data_ms_ = now;
                // A reading with no pulse in it is still a reading (the link
                // is alive), but its bpm is the last one the watch had, so it
                // is published as 0 and kept out of the session.
                const bool pulse = m.pulse();
                if (pulse != (heart_.bpm != 0) || !heart_.worn) {
                    if (pulse && !heart_.worn) ESP_LOGI(kTag, "watch: pulse");
                    else if (pulse) ESP_LOGI(kTag, "watch: pulse back");
                    else if (heart_.worn) ESP_LOGI(kTag, "watch: no pulse (skin contact %s)",
                                                   m.contact_known ? (m.contact ? "yes" : "no")
                                                                   : "not reported");
                }
                if (pulse) {
                    session_.add(m.bpm);
                    heart_.worn = true;
                }
                heart_.bpm = pulse ? m.bpm : 0;
                heart_.at_ms = now != 0 ? now : 1;
                heart_.contact_known = m.contact_known;
                heart_.contact = m.contact;
                heart_.avg_bpm = session_.avg_bpm();
                heart_.max_bpm = session_.max_bpm();
                heart_.min_bpm = session_.min_bpm();
                heart_.readings = session_.count();
                ++heart_.seq;
                dirty_ = true;
                break;
            }
            case Event::Reset:
                // The host lost the controller: every connection and scan
                // is gone with it. Synced starts it all again.
                conn_ = BLE_HS_CONN_HANDLE_NONE;
                g_ctx.hr_conn.store(conn_);
                g_ctx.hr_val.store(0);
                if (step_ != Step::Off) set_step(Step::Off, now);
                break;
            default:
                break;
        }
    }

    // Once a loop, after the events.
    void poll(bool synced, uint32_t now) {
        const bool on = d_.settings->hr_on;
        if (!on && was_on_) picker_.reset();   // off and on again: any watch
        was_on_ = on;

        const uint16_t max = d_.settings->hr_max_bpm;
        if (max != heart_.zone_max_bpm) {
            heart_.zone_max_bpm = max;
            dirty_ = true;
        }

        if (on != was_logged_on_) {
            was_logged_on_ = on;
            ESP_LOGI(kTag, "watch heart rate %s", on ? "on: scanning" : "off");
        }
        if (!(on && synced)) {
            if (step_ != Step::Off) stop(now);
        } else {
            switch (step_) {
                case Step::Off:
                    wait(now, 0);
                    break;
                case Step::Waiting:
                    if (static_cast<int32_t>(now - next_ms_) >= 0) scan(now);
                    break;
                case Step::Scanning:
                    // DISC_COMPLETE ends it; this only catches one that was lost.
                    if (now - step_ms_ > core::hr::kScanMs + kHrStepMs) {
                        ble_gap_disc_cancel();
                        wait(now, 0);
                    }
                    break;
                case Step::Connecting:
                    // The host's own connect timeout ends it; this catches a lost one.
                    if (now - step_ms_ > kHrConnectMs + 2000) give_up("connect timed out", now);
                    break;
                case Step::Discovering:
                    if (now - step_ms_ > kHrStepMs) give_up("timed out", now);
                    break;
                case Step::Live:
                    if (now - last_data_ms_ > core::hr::kSilentDropMs) {
                        give_up("silent", now);
                    }
                    break;
            }
        }
        // While searching, every 30 s: whether the scans run and hear anyone.
        if (on && step_ != Step::Live && step_ != Step::Off &&
            static_cast<int32_t>(now - search_log_ms_) >= 30000) {
            search_log_ms_ = now;
            const uint32_t reports = g_ctx.scan_reports.exchange(0, std::memory_order_relaxed);
            ESP_LOGI(kTag, "watch search: %u scans started, %u finished, %" PRIu32
                     " adverts from anyone, %u heart-rate last scan",
                     scans_started_, scans_done_, reports, heard_);
            scans_started_ = scans_done_ = 0;
        }
        if (dirty_) {
            dirty_ = false;
            d_.slots->heart.publish(heart_);
        }
    }

    // With DIAG's once-a-second sample.
    void sample_rssi() {
        if (conn_ == BLE_HS_CONN_HANDLE_NONE) return;
        int8_t rssi = 0;
        if (ble_gap_conn_rssi(conn_, &rssi) == 0 && rssi != heart_.rssi_dbm) {
            heart_.rssi_dbm = rssi;
            dirty_ = true;
        }
    }

private:
    enum class Step : uint8_t { Off, Waiting, Scanning, Connecting, Discovering, Live };

    // The discovery or the subscribe that has not answered.
    static constexpr uint32_t kHrStepMs = 12000;
    // A connect: an advertising watch answers in well under a second. One
    // started as the phone reconnected sat out the whole 10 s it was given.
    static constexpr uint32_t kHrConnectMs = 4000;
    // After a dropped link or a failed connect: a moment, then look again.
    static constexpr uint32_t kRetryMs = 1000;

    void set_step(Step s, uint32_t now) {
        step_ = s;
        step_ms_ = now;
        uint8_t link = core::state::Heart::Off;
        switch (s) {
            case Step::Off:         link = core::state::Heart::Off; break;
            case Step::Waiting:
            case Step::Scanning:    link = core::state::Heart::Searching; break;
            case Step::Connecting:
            case Step::Discovering: link = core::state::Heart::Connecting; break;
            case Step::Live:        link = core::state::Heart::Live; break;
        }
        if (link != heart_.link) {
            heart_.link = link;
            dirty_ = true;
        }
    }

    void wait(uint32_t now, uint32_t ms) {
        set_step(Step::Waiting, now);
        next_ms_ = now + ms;
    }

    void scan(uint32_t now) {
        picker_.begin_scan();
        heard_ = 0;
        ++scans_started_;
        g_ctx.hr_seen_n = 0;   // the host task reads it only during a scan
        ble_gap_disc_params p{};
        p.passive = 0;             // active: the name is often in the scan response
        // The C6's controller filters duplicates by address and never forgets
        // one (CONFIG_BT_LE_SCAN_DUPL_CACHE_REFRESH_PERIOD=0 in its image): a
        // watch heard advertising for its phone before Broadcast HR was
        // turned on was never heard again. hr_disc_event dedupes per scan.
        p.filter_duplicates = 0;
        // Half the time listening, 50 of every 100 ms: back-to-back scans
        // leave the phone link and the advertising their share of the radio.
        p.itvl = 160;              // 0.625 ms units
        p.window = 80;
        const int rc = ble_gap_disc(g_ctx.own_addr_type, static_cast<int32_t>(core::hr::kScanMs),
                                    &p, hr_disc_event, nullptr);
        if (rc != 0) {
            ESP_LOGW(kTag, "watch scan would not start: %d", rc);
            wait(now, core::hr::kRescanMs);
            return;
        }
        set_step(Step::Scanning, now);
    }

    void connect_to_choice(uint32_t now) {
        core::hr::Addr a;
        if (!picker_.choice(a)) {
            // Nobody: listen again at once, so a watch whose broadcast is
            // turned on later is found within a scan of it.
            wait(now, 0);
            return;
        }
        target_ = a;
        std::snprintf(heart_.name, sizeof heart_.name, "%s",
                      picker_.choice_name()[0] ? picker_.choice_name()
                      : picker_.latched()      ? picker_.name() : "");
        ble_addr_t peer{};
        peer.type = a.type;
        std::memcpy(peer.val, a.val, sizeof peer.val);
        g_ctx.hr_peer_valid.store(false);
        g_ctx.hr_peer = peer;
        g_ctx.hr_peer_valid.store(true);
        const int rc = ble_gap_connect(g_ctx.own_addr_type, &peer,
                                       static_cast<int32_t>(kHrConnectMs), nullptr,
                                       hr_conn_event, nullptr);
        if (rc != 0) {
            ESP_LOGW(kTag, "watch connect would not start: %d", rc);
            wait(now, core::hr::kRescanMs);
            return;
        }
        set_step(Step::Connecting, now);
        dirty_ = true;   // the name
    }

    void subscribe(uint32_t now) {
        static const uint8_t kOn[2] = {0x01, 0x00};   // notifications
        if (ble_gattc_write_flat(conn_, cccd_, kOn, sizeof kOn, hr_write_cb, nullptr) != 0) {
            give_up("subscribe would not start", now);
        }
    }

    // Drop whatever step this is and look again later. A live link is let go
    // and the Disconnect brings it back to Waiting.
    void give_up(const char* why, uint32_t now) {
        ESP_LOGW(kTag, "watch: %s", why);
        if (step_ == Step::Connecting) ble_gap_conn_cancel();
        if (conn_ != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn_, BLE_ERR_REM_USER_CONN_TERM);
        }
        wait(now, core::hr::kRescanMs);
    }

    void stop(uint32_t now) {
        if (step_ == Step::Scanning) ble_gap_disc_cancel();
        if (step_ == Step::Connecting) ble_gap_conn_cancel();
        if (conn_ != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn_, BLE_ERR_REM_USER_CONN_TERM);
        }
        heart_.rssi_dbm = 0;
        set_step(Step::Off, now);
        dirty_ = true;
    }

    Deps&    d_;
    core::state::Heart heart_;
    core::hr::Picker   picker_;
    core::hr::Session  session_;
    core::hr::Addr     target_{};
    Step     step_ = Step::Off;
    uint32_t step_ms_ = 0;
    uint32_t next_ms_ = 0;
    uint32_t last_data_ms_ = 0;
    uint16_t conn_ = BLE_HS_CONN_HANDLE_NONE;
    uint16_t svc_start_ = 0, svc_end_ = 0, val_ = 0, cccd_ = 0;
    bool     secured_ = false;
    bool     was_on_ = false;
    bool     was_logged_on_ = false;
    unsigned heard_ = 0;            // HrAdv this scan
    unsigned scans_started_ = 0;    // since the last search log
    unsigned scans_done_ = 0;
    uint32_t search_log_ms_ = 0;
    unsigned heard_logged_ = ~0u;   // what the log last said
    bool     dirty_ = false;
};

// The iPhone's AMS and ANCS (§11.1.1, core/media/apple), from this task's
// side. Once a phone is let in, its services are looked through; an Android
// phone has neither, this goes quiet, and the feed is all there is. One GATT
// procedure at a time, each started here and answered through the queue:
// find the services, then their characteristics, then subscribe step by
// step, then one write at a time (a handlebar press, or ANCS asking for a
// notification's text).
class AppleLink {
public:
    // What one event moved into the media and notify slots.
    struct Out {
        char tag = 0;          // a media field changed: its §11.1 tag
        bool notify = false;   // the notification changed
    };

    void start(uint16_t conn, uint32_t now) {
        *this = AppleLink{};
        conn_ = conn;
        set_step(Step::Services, now);
        if (ble_gattc_disc_all_svcs(conn_, apple_svc_cb, nullptr) != 0) {
            give_up("service discovery would not start");
        }
    }

    void stop() { *this = AppleLink{}; }

    void on_event(const Event& e, uint32_t now, core::state::Media& media,
                  core::state::Notify& notify, Out& out) {
        if (conn_ == BLE_HS_CONN_HANDLE_NONE || e.conn != conn_) return;
        switch (e.kind) {
            case Event::AppleSvc:
                if (step_ != Step::Services) break;
                if (e.code == 0) {
                    Range& r = e.which == kAmsSvc ? ams_ : ancs_;
                    r.start = e.h1;
                    r.end = e.h2;
                } else if (ams_.start == 0 && ancs_.start == 0) {
                    // An Android phone, or an iPhone that would not say.
                    set_step(Step::Off, now);
                } else {
                    ESP_LOGI(kTag, "iPhone: %s%s%s", ams_.start ? "AMS" : "",
                             ams_.start && ancs_.start ? " and " : "", ancs_.start ? "ANCS" : "");
                    next_chars(now);
                }
                break;
            case Event::AppleChr:
                if (step_ != Step::Chars) break;
                if (e.code == 0) {
                    switch (e.which) {
                        case kAmsRemote:   remote_ = e.h1; break;
                        case kAmsEntity:   entity_ = e.h1; break;
                        case kAncsSource:  source_ = e.h1; break;
                        case kAncsControl: control_ = e.h1; break;
                        case kAncsData:    data_ = e.h1; break;
                        default: break;
                    }
                } else {
                    next_chars(now);
                }
                break;
            case Event::AppleDsc:
                if (step_ != Step::Subscribing || ops_[op_].kind != Op::Cccd) break;
                if (e.code == 0) {
                    if (cccd_ == 0) cccd_ = e.h1;
                } else {
                    // None found: the CCCD is conventionally the next handle.
                    if (cccd_ == 0) cccd_ = static_cast<uint16_t>(ops_[op_].handle + 1);
                    static const uint8_t kOn[2] = {0x01, 0x00};   // notifications
                    write(cccd_, kOn, sizeof kOn, now);
                }
                break;
            case Event::AppleWrite:
                if (step_ == Step::Subscribing) {
                    if (e.code != 0) {
                        // Usually ANCS before the rider has allowed it in the
                        // iPhone's prompt. That service is off for this link.
                        ESP_LOGW(kTag, "iPhone: %s refused (%" PRId32 ")",
                                 ops_[op_].ams ? "AMS" : "ANCS", e.code);
                        (ops_[op_].ams ? ams_ok_ : ancs_ok_) = false;
                    }
                    ++op_;
                    run_op(now);
                } else if (step_ == Step::Live) {
                    if (e.code != 0) ESP_LOGW(kTag, "iPhone write refused (%" PRId32 ")", e.code);
                    busy_ = false;
                }
                break;
            case Event::AppleData:
                if (e.h1 == 0) break;
                if (e.h1 == entity_ && ams_ok_) {
                    out.tag = core::media::ams::apply_update(media, e.data, e.len, now);
                } else if (e.h1 == source_ && ancs_ok_) {
                    out.notify = tracker_.on_source(notify, e.data, e.len, now);
                } else if (e.h1 == data_ && ancs_ok_) {
                    out.notify = tracker_.on_data(notify, e.data, e.len, now);
                }
                break;
            default:
                break;
        }
    }

    // Once a loop: a step that never answered, and the next write.
    void poll(uint32_t now) {
        if (step_ == Step::Services || step_ == Step::Chars || step_ == Step::Subscribing) {
            if (now - step_ms_ > kStepMs) give_up("timed out");
            return;
        }
        if (step_ != Step::Live || busy_) return;
        if (press_ != kNoPress) {
            write(remote_, &press_, 1, now);
            press_ = kNoPress;
            return;
        }
        if (!ancs_ok_) return;
        uint8_t req[core::media::ancs::kRequestLen];
        const size_t n = tracker_.next_request(req, sizeof req, now);
        if (n != 0 && !write(control_, req, n, now)) tracker_.unask();
    }

    // A handlebar press. False if the phone has no AMS, so the feed's Ctrl
    // should carry it instead.
    bool press(uint8_t cmd) {
        if (step_ != Step::Live || !ams_ok_ || remote_ == 0) return false;
        uint8_t id;
        if (core::media::ams::remote_command(cmd, id)) press_ = id;
        return true;
    }

    // For DIAG (BleDiag::apple): bit 0 looking, bit 1 AMS live, bit 2 ANCS live.
    uint8_t diag() const {
        if (step_ == Step::Live) return static_cast<uint8_t>((ams_ok_ ? 2 : 0) | (ancs_ok_ ? 4 : 0));
        return step_ == Step::Off ? 0 : 1;
    }

private:
    enum class Step : uint8_t { Off, Services, Chars, Subscribing, Live };
    struct Range { uint16_t start = 0, end = 0; };
    struct Op {
        enum Kind : uint8_t { Cccd, Write } kind;
        bool           ams;      // which service it belongs to
        uint16_t       handle;   // the characteristic's value
        const uint8_t* data;     // Write
        uint8_t        len;
        uint16_t       end;      // Cccd: where to stop looking for it
    };
    static constexpr uint32_t kStepMs = 10000;
    static constexpr uint8_t kNoPress = 0xFF;

    void set_step(Step s, uint32_t now) {
        step_ = s;
        step_ms_ = now;
    }

    void give_up(const char* why) {
        ESP_LOGW(kTag, "iPhone: %s", why);
        const uint16_t conn = conn_;
        stop();
        conn_ = conn;   // still the phone's link: the feed may yet come
    }

    // Each service's characteristics in turn, AMS first.
    void next_chars(uint32_t now) {
        Range* r = !ams_done_ && ams_.start ? &ams_ : !ancs_done_ && ancs_.start ? &ancs_ : nullptr;
        if (r == nullptr) {
            plan(now);
            return;
        }
        (r == &ams_ ? ams_done_ : ancs_done_) = true;
        set_step(Step::Chars, now);
        if (ble_gattc_disc_all_chrs(conn_, r->start, r->end, apple_chr_cb, nullptr) != 0) {
            give_up("characteristic discovery would not start");
        }
    }

    // The subscriptions, in order. AMS: its Entity Update CCCD, then the two
    // writes that say what to send. ANCS: Data Source before Notification
    // Source, as Apple asks, so no answer can arrive unsubscribed.
    void plan(uint32_t now) {
        using core::media::ams::kSubscribePlayer;
        using core::media::ams::kSubscribeTrack;
        ams_ok_ = ams_.start != 0 && entity_ != 0;
        ancs_ok_ = ancs_.start != 0 && source_ != 0 && control_ != 0 && data_ != 0;
        n_ops_ = 0;
        if (ams_ok_) {
            ops_[n_ops_++] = Op{Op::Cccd, true, entity_, nullptr, 0, ams_.end};
            ops_[n_ops_++] = Op{Op::Write, true, entity_, kSubscribeTrack, sizeof kSubscribeTrack, 0};
            ops_[n_ops_++] = Op{Op::Write, true, entity_, kSubscribePlayer, sizeof kSubscribePlayer, 0};
        }
        if (ancs_ok_) {
            ops_[n_ops_++] = Op{Op::Cccd, false, data_, nullptr, 0, ancs_.end};
            ops_[n_ops_++] = Op{Op::Cccd, false, source_, nullptr, 0, ancs_.end};
        }
        if (n_ops_ == 0) {
            ESP_LOGW(kTag, "iPhone: services without their characteristics");
            set_step(Step::Off, now);
            return;
        }
        op_ = 0;
        set_step(Step::Subscribing, now);
        run_op(now);
    }

    void run_op(uint32_t now) {
        // An op whose service was refused on the way is skipped.
        while (op_ < n_ops_ && !(ops_[op_].ams ? ams_ok_ : ancs_ok_)) ++op_;
        if (op_ == n_ops_) {
            if (ams_ok_ || ancs_ok_) {
                ESP_LOGI(kTag, "iPhone live:%s%s", ams_ok_ ? " media" : "",
                         ancs_ok_ ? " notifications" : "");
                set_step(Step::Live, now);
            } else {
                set_step(Step::Off, now);
            }
            return;
        }
        step_ms_ = now;
        const Op& op = ops_[op_];
        if (op.kind == Op::Write) {
            write(op.handle, op.data, op.len, now);
            return;
        }
        cccd_ = 0;
        if (op.handle >= op.end ||
            ble_gattc_disc_all_dscs(conn_, op.handle, op.end, apple_dsc_cb, nullptr) != 0) {
            cccd_ = static_cast<uint16_t>(op.handle + 1);
            static const uint8_t kOn[2] = {0x01, 0x00};
            write(cccd_, kOn, sizeof kOn, now);
        }
    }

    bool write(uint16_t handle, const uint8_t* data, size_t len, uint32_t now) {
        if (ble_gattc_write_flat(conn_, handle, data, static_cast<uint16_t>(len),
                                 apple_write_cb, nullptr) != 0) {
            if (step_ != Step::Live) give_up("write would not start");
            return false;
        }
        busy_ = true;
        step_ms_ = now;
        return true;
    }

    uint16_t conn_ = BLE_HS_CONN_HANDLE_NONE;
    Step     step_ = Step::Off;
    uint32_t step_ms_ = 0;
    Range    ams_, ancs_;
    bool     ams_done_ = false, ancs_done_ = false;
    bool     ams_ok_ = false, ancs_ok_ = false;
    uint16_t remote_ = 0, entity_ = 0, source_ = 0, control_ = 0, data_ = 0;
    Op       ops_[5] = {};
    uint8_t  n_ops_ = 0, op_ = 0;
    uint16_t cccd_ = 0;
    bool     busy_ = false;
    uint8_t  press_ = kNoPress;
    core::media::ancs::Tracker tracker_;
};

void ble_task(void* arg) {
    using core::state::Dev;
    Deps& d = *static_cast<Deps*>(arg);

    demote_radio_tasks(true);
    // Before NimBLE starts: its store loads, and may write, at sync.
    g_ctx.still.store(still_for_flash(d));
    core::state::BleDiag diag;
    if (!bring_up(d, diag.c6_fw, sizeof diag.c6_fw)) {
        d.slots->ble_diag.publish(diag);   // whatever was learned before it failed
        // Nothing to retry into: a C6 that will not answer at boot is a
        // module problem, not a loose connector. DIAG shows the fault.
        vTaskDelete(nullptr);
        return;
    }
    // The RPC tasks only exist once the link is up.
    demote_radio_tasks(true);

    core::state::Media media;
    // The phone's last notification (§11.1 `N`): this task is its one writer.
    core::state::Notify notify;
    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    bool subscribed = false;
    bool synced = false;
    bool radio_lost = false;   // for good: ESP-Hosted does not bring it back
    uint32_t dropped_at_disconnect_ms = 0;   // 0: nothing to clear
    uint32_t ctrl_gen = d.slots->media_ctrl.generation();
    uint32_t request_gen = d.slots->ble_request.generation();

    // Who may connect (§11.3). The policy is core::ble; this only acts on it.
    core::ble::Peers peers;
    core::ble::PairingWindow window;
    bool     encrypted = false;
    bool     dropping = false;      // terminate sent, waiting for Disconnect
    bool     forget_pending = false;  // Forget phone, waiting for the bike to stop
    uint32_t connected_ms = 0;
    uint32_t last_diag_ms = 0;
    uint32_t dropped_seen = 0;

    // The watch's heart rate (core/hr). Its state is a few hundred bytes,
    // so it lives off the task stack.
    static HrLink hr(d);
    hr.publish_initial();
    // The iPhone's AMS and ANCS, off the stack for the same reason.
    static AppleLink apple;

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
            hr.on_event(e, now);
            AppleLink::Out out;
            apple.on_event(e, now, media, notify, out);
            if (out.tag) {
                diag.last_tag = out.tag;
                media_dirty = diag_dirty = true;
                d.health->report_ok(Dev::Phone, now);
            }
            if (out.notify) {
                ESP_LOGI(kTag, "notification %c from \"%s\"%s (ANCS)", notify.kind, notify.who,
                         notify.ringing ? ", ringing" : "");
                diag.last_tag = 'N';
                diag_dirty = true;
                d.slots->notify.publish(notify);
                d.health->report_ok(Dev::Phone, now);
            }
            switch (e.kind) {
                case Event::Line:
                    if (const char tag = core::media::apply_line(media, e.data, e.len, now)) {
                        diag.last_tag = tag;
                        media_dirty = diag_dirty = true;
                        d.health->report_ok(Dev::Phone, now);
                    } else if (core::media::apply_notify(notify, e.data, e.len, now)) {
                        // One line each, so a call that never shows can be
                        // traced to this end or the phone's (logcat DashNotify).
                        ESP_LOGI(kTag, "notification %c from \"%s\"%s", notify.kind, notify.who,
                                 notify.ringing ? ", ringing" : "");
                        diag.last_tag = 'N';
                        diag_dirty = true;
                        d.slots->notify.publish(notify);
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
                    apple.stop();
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
                    // A call cannot ring on a phone the dash cannot hear.
                    if (notify.ringing) {
                        notify.ringing = false;
                        d.slots->notify.publish(notify);
                    }
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
                            if (!known) {
                                load_peers(peers);
                                if (!peers.contains(e.peer)) {
                                    // Welcome, but the store gate held the
                                    // bond back (the bike was moving). The
                                    // phone thinks it is bonded and the
                                    // dash does not; drop it, keep the
                                    // window, and let it pair again parked.
                                    ESP_LOGW(kTag, "new phone paired while moving, bond not saved; pair again when stopped");
                                    drop(conn, BLE_ERR_AUTH_FAIL);
                                    dropping = true;
                                    break;
                                }
                                encrypted = true;
                                ESP_LOGI(kTag, "new phone bonded");
                                window.close();   // one phone per window
                            } else {
                                encrypted = true;
                                ESP_LOGI(kTag, "bonded phone back");
                            }
                            break;
                        case core::ble::Verdict::Reject: {
                            // The store gate never wrote its bond, so there
                            // is nothing to delete (and so no flash write).
                            ESP_LOGW(kTag, "unknown phone outside pairing mode, refused");
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
                    // Let in: an iPhone serves AMS and ANCS from here.
                    if (encrypted) apple.start(conn, now);
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
                case Event::RadioLost:
                    if (radio_lost) break;      // one per failed transfer
                    radio_lost = true;
                    ESP_LOGE(kTag, "radio lost: the C6 stopped answering; "
                                   "Bluetooth is off until the next power-on");
                    // NimBLE goes on asking the dead controller every 2 s;
                    // the console does not need to hear each one.
                    esp_log_level_set("NimBLE", ESP_LOG_NONE);
                    esp_log_level_set("H_SDIO_DRV", ESP_LOG_NONE);
                    esp_log_level_set("sdmmc_io", ESP_LOG_NONE);
                    synced = false;
                    conn = BLE_HS_CONN_HANDLE_NONE;
                    g_ctx.phone_links.store(0);
                    apple.stop();
                    subscribed = false;
                    encrypted = false;
                    diag.ctrl_subscribed = false;
                    diag.encrypted = false;
                    d.health->set_absent(Dev::Phone);
                    break;
                case Event::Reset:
                    // NimBLE resyncs with the controller on its own and
                    // sends Synced again when it has.
                    ESP_LOGW(kTag, "host reset, reason %" PRId32, e.code);
                    synced = false;
                    conn = BLE_HS_CONN_HANDLE_NONE;
                    apple.stop();
                    subscribed = false;
                    encrypted = false;
                    dropping = false;
                    diag.ctrl_subscribed = false;
                    diag.encrypted = false;
                    d.health->report_error(Dev::BleHost, e.code, now);
                    d.health->set_absent(Dev::Phone);
                    dropped_at_disconnect_ms = now != 0 ? now : 1;
                    break;
                default:
                    break;   // the watch's: HrLink's
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
                    // The clear itself is a flash write, so it waits for
                    // the bike to be still (below); the link goes now.
                    ESP_LOGI(kTag, "forgetting every bonded phone");
                    window.close();
                    forget_pending = true;
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

        // Still enough to write flash? The store gate asks on the host task.
        const bool still = still_for_flash(d);
        g_ctx.still.store(still);
        if (forget_pending && still) {
            forget_pending = false;
            if (ble_store_clear() != 0) ESP_LOGW(kTag, "bond store clear failed");
            load_peers(peers);
        }

        const bool open = window.is_open(now);
        g_ctx.may_evict.store(core::ble::may_evict(peers.size(), open));
        g_ctx.welcome.store(!forget_pending && core::ble::new_phone_welcome(peers.size(), open));
        const uint8_t pairing_s = static_cast<uint8_t>(window.remaining_s(now));
        const uint8_t bonds = static_cast<uint8_t>(peers.size());
        if (pairing_s != diag.pairing_s || bonds != diag.bonds) {
            diag.pairing_s = pairing_s;
            diag.bonds = bonds;
            diag_dirty = true;
        }

        // A handlebar transport press, posted by `logic`. The generation
        // says whether it is new; a press with no phone listening is
        // dropped, not queued for a phone that may never come. An iPhone
        // takes it as an AMS remote command, anything else over Ctrl.
        const uint32_t gen = d.slots->media_ctrl.generation();
        if (gen != ctrl_gen) {
            ctrl_gen = gen;
            const uint8_t cmd = d.slots->media_ctrl.get().cmd;
            if (conn != BLE_HS_CONN_HANDLE_NONE && encrypted && !apple.press(cmd) && subscribed) {
                send_ctrl(conn, cmd);
            }
        }
        apple.poll(now);
        if (apple.diag() != diag.apple) {
            diag.apple = apple.diag();
            diag_dirty = true;
        }

        if (now - last_diag_ms >= kDiagPeriodMs) {
            last_diag_ms = now;
            demote_radio_tasks(false);
            hr.sample_rssi();
            // No phone, so the dash must be findable. The callbacks restart
            // advertising when a phone drops, but a start can fail (GAP
            // preempted, the controller busy with the watch's connect), and
            // until 1.49 nothing tried again: the dash went invisible and
            // the phone waited on it forever.
            if (synced && g_ctx.phone_links.load() == 0 && !ble_gap_adv_active()) {
                start_advertising();
            }
            if (conn != BLE_HS_CONN_HANDLE_NONE) {
                int8_t rssi = 0;
                if (ble_gap_conn_rssi(conn, &rssi) == 0) {
                    diag.rssi_dbm = rssi;
                    diag.has_rssi = true;
                }
                d.health->report_ok(Dev::Phone, now);
            }
            if (synced) d.health->report_ok(Dev::BleHost, now);
            else if (radio_lost) d.health->report_error(Dev::BleHost, 6, now);
            const uint32_t dropped = g_ctx.dropped.load();
            if (dropped != dropped_seen) {
                ESP_LOGW(kTag, "%" PRIu32 " events lost to a full queue", dropped - dropped_seen);
                dropped_seen = dropped;
            }
            diag_dirty = true;
        }
        hr.poll(synced, now);
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
