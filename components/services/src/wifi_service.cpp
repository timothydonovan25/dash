// `wifi` — core 1, priority 3. The on-demand update hotspot (§11.7).
//
// Off until the rider turns it on from DIAG -> Update or CONFIG. Then the
// C6 runs a WPA2 soft AP, "CRF250F-Dash", with a new eight-digit password
// each time (shown on the Update screen), and the dash serves one page at
// http://192.168.4.1 that can
//   - upload a dash.bin into the dev slot,
//   - download the ride logs off the card,
//   - download the stable and dev firmware images, as a backup,
//   - delete ride logs,
//   - read and change every CONFIG setting,
//   - on a build that runs one (Deps::games), upload a game's .wad to games/.
// It turns itself off after core::wifi::kIdleOffMs without a request, and
// as soon as the bike moves. The policy is core::wifi; this is the radio
// and the web server.
//
// The page never writes flash itself. An upload's bytes go through
// Deps::ota_stream to `ota`, which checks the header, writes the dev slot,
// and refuses unless parked, exactly as for an install from the card.
// Booting the new image stays a button on the dash: nothing on the network
// can make the bike run code it has not been told to run.
//
// The Wi-Fi itself is on the C6, reached over the same SDIO link as BLE
// through esp_wifi_remote. A C6 still on the BLE-only build refuses to start
// Wi-Fi, and carries the same ESP-Hosted version number as the BLE + Wi-Fi
// build, so a refusal here is what triggers replacing its firmware.
#include "services/services.hpp"
#include "c6_firmware.hpp"

#include "core/config/config.hpp"
#include "core/ota/ota.hpp"
#include "core/wifi/wifi.hpp"

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace services {

extern const char kPage[];   // wifi_page.cpp

namespace {

constexpr const char* kTag = "wifi";

// Internal RAM is what the SDIO link, lwIP and NimBLE all draw on, and it
// is what ran out the first time an upload went through. Logged at every
// change of state so a leak shows as a falling number.
void log_ram(const char* when) {
    ESP_LOGI(kTag, "%s: internal free %u, largest %u, low %u", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
}
constexpr uint32_t kTickMs = 250;
constexpr size_t kChunk = 4096;

// Any other failure is believed on the second try: the first can be the
// C6 still settling after BLE came up. ESP_ERR_NOT_SUPPORTED is the C6
// saying it was built without Wi-Fi, and is believed at once.
constexpr int kRefusalsBeforeC6Update = 2;

// How long an upload waits for `ota` to finish checking and writing after
// the last byte arrives. esp_ota_end hashes the whole image.
constexpr uint32_t kInstallWaitMs = 60000;

// Shared between the wifi task and the web server's task. The server only
// reads Deps and stamps `last_use_ms`; the wifi task owns everything else.
struct Web {
    Deps* d = nullptr;
    std::atomic<uint32_t> last_use_ms{0};
    std::atomic<uint8_t> clients{0};
    void touch() { last_use_ms.store(d->clock->now_ms()); }
};
Web g_web;
// One buffer for the server's one task: uploads and downloads both pass
// through it a chunk at a time. PSRAM, as ota's g_chunk: the radio is the
// bottleneck, not the copy through IDF's bounce buffers.
EXT_RAM_BSS_ATTR uint8_t g_buf[kChunk];
// A game upload's pieces go to the card this big: each write_at opens the
// file and seeks to its end, and 4 kB at a time was a thousand of those for
// one WAD.
constexpr size_t kGameChunk = 32 * 1024;
EXT_RAM_BSS_ATTR uint8_t g_game_buf[kGameChunk];
// Big enough for any IWAD people have (DOOM2.WAD 14 MB, Freedoom's 28 MB).
constexpr size_t kGameMaxBytes = 64u * 1024 * 1024;

// --- page handlers --------------------------------------------------------

esp_err_t send_json(httpd_req_t* r, const char* body) {
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

esp_err_t page_get(httpd_req_t* r) {
    g_web.touch();
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_sendstr(r, kPage);
}

// Stable is whichever slot the bootloader would fall back to: the running
// one when stable is running, the other one when dev is.
const esp_partition_t* slot_for(bool stable) {
    const core::state::Ota o = g_web.d->slots->ota.get();
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(running);
    const bool running_is_stable = !o.known || o.role == core::ota::Role::Stable;
    return stable == running_is_stable ? running : other;
}

esp_err_t status_get(httpd_req_t* r) {
    g_web.touch();
    const core::state::Ota o = g_web.d->slots->ota.get();
    const hal::ICard* card = g_web.d->card;
    char body[448];
    std::snprintf(body, sizeof body,
                  "{\"running\":\"%s\",\"running_date\":\"%s\",\"slot\":\"%s\","
                  "\"role\":\"%s\",\"other\":\"%s\",\"other_date\":\"%s\","
                  "\"install\":%u,\"pct\":%u,\"error\":\"%s\","
                  "\"image_error\":\"%s\",\"card\":%s,\"games\":%s}",
                  o.running_ver, o.running_date, o.running_slot, core::ota::role_name(o.role),
                  o.other == core::ota::Other::Ready ? o.other_ver : "",
                  o.other == core::ota::Other::Ready ? o.other_date : "",
                  static_cast<unsigned>(o.install), static_cast<unsigned>(o.install_pct),
                  core::ota::install_error_name(o.error),
                  core::ota::image_error_name(o.image_error),
                  card && card->present() ? "true" : "false",
                  g_web.d->games ? "true" : "false");
    return send_json(r, body);
}

// Firmware out: the image only, not the 6 MB slot it sits in.
esp_err_t fw_get(httpd_req_t* r) {
    g_web.touch();
    const bool stable = std::strcmp(r->uri, "/fw/stable.bin") == 0;
    const esp_partition_t* p = slot_for(stable);
    esp_image_metadata_t md = {};
    const esp_partition_pos_t pos = {p ? p->address : 0, p ? p->size : 0};
    if (!p || esp_image_get_metadata(&pos, &md) != ESP_OK || md.image_len == 0) {
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no image in that slot");
    }
    // Named like a build: dash-<version>-<date>-<board>-<feature>.bin.
    esp_app_desc_t desc = {};
    char date[11] = "";
    if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
        core::ota::iso_date(desc.date, date, sizeof date);
    }
    char file[64];
    core::ota::bin_name(desc.version, date, file, sizeof file);
    char disp[96];
    std::snprintf(disp, sizeof disp, "attachment; filename=\"%s\"", file);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    for (size_t at = 0; at < md.image_len;) {
        const size_t n = md.image_len - at < kChunk ? md.image_len - at : kChunk;
        if (esp_partition_read(p, at, g_buf, n) != ESP_OK ||
            httpd_resp_send_chunk(r, reinterpret_cast<const char*>(g_buf), n) != ESP_OK) {
            httpd_resp_send_chunk(r, nullptr, 0);
            return ESP_FAIL;
        }
        at += n;
        g_web.touch();
    }
    return httpd_resp_send_chunk(r, nullptr, 0);
}

// The log list as JSON. Built into g_buf while the card lock is held and
// sent after it is released: sending from inside the listing held the card
// lock for as long as the phone took to read the response, and the logger,
// which gives up on the lock after 3 s, lost a block of rows each time.
// A directory bigger than the buffer goes in pages, one listing each.
struct ListCtx {
    char*  buf;
    size_t cap;
    size_t len = 0;
    size_t skip = 0;      // valid entries already sent by earlier pages
    size_t seen = 0;
    size_t taken = 0;
    bool   full = false;
    bool   first = true;  // no entry sent yet, so no leading comma
    bool (*valid)(const char*) = core::wifi::valid_log_name;
};

void list_one(const char* name, size_t size, void* vctx) {
    auto* c = static_cast<ListCtx*>(vctx);
    if (!c->valid(name) || c->full) return;
    if (c->seen++ < c->skip) return;
    char item[96];
    const int n = std::snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"size\":%u}",
                                c->first ? "" : ",", name, static_cast<unsigned>(size));
    if (n <= 0 || c->len + static_cast<size_t>(n) > c->cap) {
        c->full = true;
        return;
    }
    std::memcpy(c->buf + c->len, item, static_cast<size_t>(n));
    c->len += static_cast<size_t>(n);
    c->first = false;
    ++c->taken;
}

// One directory's valid names as a JSON array, a page of the listing at a time.
esp_err_t list_dir(httpd_req_t* r, const char* dir, bool (*valid)(const char*)) {
    g_web.touch();
    hal::ICard* card = g_web.d->card;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, "[", 1);
    if (card && card->present()) {
        size_t skip = 0;
        bool first = true;
        // A page is ~90 entries; 64 pages is more logs than a card holds.
        for (int page = 0; page < 64; ++page) {
            ListCtx ctx;
            ctx.buf = reinterpret_cast<char*>(g_buf);
            ctx.cap = kChunk;
            ctx.skip = skip;
            ctx.first = first;
            ctx.valid = valid;
            card->list(dir, list_one, &ctx);
            if (ctx.len) httpd_resp_send_chunk(r, ctx.buf, static_cast<ssize_t>(ctx.len));
            g_web.touch();
            skip += ctx.taken;
            first = ctx.first;
            if (!ctx.full || ctx.taken == 0) break;
        }
    }
    httpd_resp_send_chunk(r, "]", 1);
    return httpd_resp_send_chunk(r, nullptr, 0);
}

esp_err_t logs_list_get(httpd_req_t* r) {
    return list_dir(r, "logs", core::wifi::valid_log_name);
}

esp_err_t log_get(httpd_req_t* r) {
    g_web.touch();
    const char* name = r->uri + std::strlen("/logs/");
    hal::ICard* card = g_web.d->card;
    if (!core::wifi::valid_log_name(name)) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not a log name");
    }
    char path[48];
    std::snprintf(path, sizeof path, "logs/%s", name);
    size_t size = 0;
    if (!card || !hal::ok(card->size_of(path, size))) {
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no such log");
    }
    char disp[64];
    std::snprintf(disp, sizeof disp, "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(r, "text/csv");
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    for (size_t at = 0; at < size;) {
        size_t got = 0;
        const size_t want = size - at < kChunk ? size - at : kChunk;
        if (!hal::ok(card->read_at(path, at, g_buf, want, got)) || got == 0 ||
            httpd_resp_send_chunk(r, reinterpret_cast<const char*>(g_buf), got) != ESP_OK) {
            httpd_resp_send_chunk(r, nullptr, 0);
            return ESP_FAIL;
        }
        at += got;
        g_web.touch();
    }
    return httpd_resp_send_chunk(r, nullptr, 0);
}

esp_err_t send_status(httpd_req_t* r, const char* status, const char* body) {
    httpd_resp_set_status(r, status);
    return send_json(r, body);
}

esp_err_t log_delete(httpd_req_t* r) {
    g_web.touch();
    const char* name = r->uri + std::strlen("/logs/");
    hal::ICard* card = g_web.d->card;
    if (!core::wifi::valid_log_name(name)) {
        return send_status(r, "400 Bad Request", "{\"ok\":false,\"error\":\"not a log name\"}");
    }
    if (!card || !card->present()) {
        return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no card\"}");
    }
    char path[48];
    std::snprintf(path, sizeof path, "logs/%s", name);
    const hal::Status st = card->remove(path);
    if (st == hal::Status::Busy) {
        return send_status(r, "409 Conflict",
                           "{\"ok\":false,\"error\":\"the dash is writing that log\"}");
    }
    if (!hal::ok(st)) {
        return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no such log\"}");
    }
    ESP_LOGI(kTag, "deleted %s", path);
    return send_json(r, "{\"ok\":true}");
}

// --- games ---------------------------------------------------------------
//
// Only on a build with Deps::games. A .wad goes to games/upload.tmp and is
// renamed over the old one only once every byte is on the card, so a cut-off
// upload leaves the game that was there. The card is not flash: nothing here
// stops core 0, and the hotspot is off whenever the bike moves anyway.

constexpr const char* kGameTmp = "games/upload.tmp";

esp_err_t games_list_get(httpd_req_t* r) {
    if (!g_web.d->games) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no games");
    return list_dir(r, "games", core::wifi::valid_game_name);
}

esp_err_t game_put(httpd_req_t* r) {
    g_web.touch();
    Deps& d = *g_web.d;
    const char* name = r->uri + std::strlen("/games/");
    if (!d.games) return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no games\"}");
    if (!core::wifi::valid_game_name(name)) {
        return send_status(r, "400 Bad Request", "{\"ok\":false,\"error\":\"not a .wad name\"}");
    }
    if (!d.card || !d.card->present()) {
        return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no card\"}");
    }
    const size_t size = r->content_len;
    if (size == 0 || size > kGameMaxBytes) {
        return send_status(r, "400 Bad Request", "{\"ok\":false,\"error\":\"too big for a WAD\"}");
    }

    size_t at = 0;     // bytes on the card
    size_t held = 0;   // bytes in g_game_buf, not yet written
    while (at + held < size) {
        const size_t room = kGameChunk - held;
        const size_t left = size - at - held;
        const int n = httpd_req_recv(r, reinterpret_cast<char*>(g_game_buf + held),
                                     left < room ? left : room);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            ESP_LOGW(kTag, "game upload cut off at %u of %u", static_cast<unsigned>(at + held),
                     static_cast<unsigned>(size));
            d.card->remove(kGameTmp);
            return ESP_FAIL;
        }
        held += static_cast<size_t>(n);
        g_web.touch();
        if (held == kGameChunk || at + held == size) {
            const hal::Status st = d.card->write_at(kGameTmp, at, g_game_buf, held);
            if (!hal::ok(st)) {
                ESP_LOGW(kTag, "game upload: card write at %u: %s", static_cast<unsigned>(at),
                         hal::status_name(st));
                d.card->remove(kGameTmp);
                return send_status(r, "500 Internal Server Error",
                                   "{\"ok\":false,\"error\":\"the card refused it\"}");
            }
            at += held;
            held = 0;
        }
    }

    char path[48];
    std::snprintf(path, sizeof path, "games/%s", name);
    d.card->remove(path);   // NotPresent for a first upload, which is fine
    if (!hal::ok(d.card->rename(kGameTmp, path))) {
        d.card->remove(kGameTmp);
        return send_status(r, "500 Internal Server Error",
                           "{\"ok\":false,\"error\":\"could not rename it on the card\"}");
    }
    ESP_LOGI(kTag, "game %s: %u bytes", path, static_cast<unsigned>(size));
    return send_json(r, "{\"ok\":true}");
}

esp_err_t game_delete(httpd_req_t* r) {
    g_web.touch();
    const char* name = r->uri + std::strlen("/games/");
    hal::ICard* card = g_web.d->card;
    if (!g_web.d->games || !core::wifi::valid_game_name(name)) {
        return send_status(r, "400 Bad Request", "{\"ok\":false,\"error\":\"not a .wad name\"}");
    }
    if (!card || !card->present()) {
        return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no card\"}");
    }
    char path[48];
    std::snprintf(path, sizeof path, "games/%s", name);
    if (!hal::ok(card->remove(path))) {
        return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no such file\"}");
    }
    ESP_LOGI(kTag, "deleted %s", path);
    return send_json(r, "{\"ok\":true}");
}

// --- settings -------------------------------------------------------------
//
// The same table CONFIG draws (core::config), so the page cannot offer a
// setting the dash does not have or a range the dash would not accept.
// Actions are left out: each is a button on the dash for a reason, and one
// of them turns this hotspot off.

const char* kind_name(core::config::Kind k) {
    switch (k) {
        case core::config::Kind::Float: return "float";
        case core::config::Kind::Int:   return "int";
        case core::config::Kind::Bool:  return "bool";
        case core::config::Kind::Enum:  return "enum";
        default:                        return "action";
    }
}

esp_err_t settings_get(httpd_req_t* r) {
    namespace cfg = core::config;
    g_web.touch();
    const cfg::Settings* s = g_web.d->settings;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, "[", 1);
    bool first = true;
    for (size_t i = 0; s && i < cfg::item_count(); ++i) {
        const cfg::ConfigItem& it = cfg::items()[i];
        if (it.kind == cfg::Kind::Action) continue;
        char item[320];
        int n = std::snprintf(item, sizeof item,
                              "%s{\"key\":%u,\"group\":\"%s\",\"label\":\"%s\","
                              "\"kind\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g,"
                              "\"unit\":\"%s\",\"value\":%g",
                              first ? "" : ",", static_cast<unsigned>(it.key), it.group,
                              it.label, kind_name(it.kind), static_cast<double>(it.min),
                              static_cast<double>(it.max), static_cast<double>(it.step),
                              it.unit, static_cast<double>(cfg::get(*s, it)));
        if (it.kind == cfg::Kind::Enum) {
            n += std::snprintf(item + n, sizeof item - n, ",\"options\":[");
            for (uint8_t o = 0; o < it.option_count && n < static_cast<int>(sizeof item); ++o) {
                n += std::snprintf(item + n, sizeof item - n, "%s\"%s\"", o ? "," : "",
                                   it.options[o]);
            }
            n += std::snprintf(item + n, sizeof item - n, "]");
        }
        n += std::snprintf(item + n, sizeof item - n, "}");
        if (n >= static_cast<int>(sizeof item)) continue;   // never, but never torn JSON
        first = false;
        httpd_resp_send_chunk(r, item, n);
    }
    httpd_resp_send_chunk(r, "]", 1);
    return httpd_resp_send_chunk(r, nullptr, 0);
}

// POST /api/set?key=257&value=80. The change goes to `logic` as the same
// ConfigSet the config screen posts, so it is applied, clamped, saved to
// NVS and picked up by every module in exactly the same way. The answer is
// the value after the table's clamp, which is what the dash will hold.
esp_err_t settings_set(httpd_req_t* r) {
    namespace cfg = core::config;
    g_web.touch();
    char q[64], key_s[12], val_s[24];
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK ||
        httpd_query_key_value(q, "key", key_s, sizeof key_s) != ESP_OK ||
        httpd_query_key_value(q, "value", val_s, sizeof val_s) != ESP_OK) {
        return send_status(r, "400 Bad Request", "{\"ok\":false,\"error\":\"key and value\"}");
    }
    char* end = nullptr;
    const unsigned long key = std::strtoul(key_s, &end, 0);
    const cfg::ConfigItem* it = (end && *end == '\0' && key <= 0xFFFF)
                                    ? cfg::find(static_cast<uint16_t>(key)) : nullptr;
    if (!it || it->kind == cfg::Kind::Action) {
        return send_status(r, "404 Not Found", "{\"ok\":false,\"error\":\"no such setting\"}");
    }
    const float v = std::strtof(val_s, &end);
    if (!end || *end != '\0' || v != v) {
        return send_status(r, "400 Bad Request", "{\"ok\":false,\"error\":\"not a number\"}");
    }
    const float clamped = cfg::clamp(*it, v);
    if (!g_web.d->commands ||
        !g_web.d->commands->post(core::state::Command::config_set(it->key, clamped))) {
        return send_status(r, "503 Service Unavailable",
                           "{\"ok\":false,\"error\":\"the dash is busy, try again\"}");
    }
    char body[64];
    std::snprintf(body, sizeof body, "{\"ok\":true,\"value\":%g}", static_cast<double>(clamped));
    return send_json(r, body);
}

// Firmware in. The body goes to `ota` through the stream; the answer is
// whatever `ota` made of it.
esp_err_t fw_post(httpd_req_t* r) {
    g_web.touch();
    Deps& d = *g_web.d;
    auto* sb = static_cast<StreamBufferHandle_t>(d.ota_stream);
    if (!sb) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no stream");
    const size_t size = r->content_len;
    if (size < core::ota::kHeaderBytes || size > 0x600000) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not a dash.bin");
    }

    // Not while an install is under way. One whose upload was cut off is
    // still waiting on the stream (ota gives up after 10 s of silence), and
    // xStreamBufferReset does nothing while a reader is blocked on it: a
    // retry inside that window fed its bytes to the old install as the rest
    // of the old image, both failed, and this request could be answered
    // with the old one's verdict. The page says to try again shortly.
    {
        const core::ota::Install now = d.slots->ota.get().install;
        if (now == core::ota::Install::Checking || now == core::ota::Install::Writing) {
            return send_status(r, "409 Conflict",
                               "{\"ok\":false,\"error\":\"an install is still running, "
                               "try again in a few seconds\"}");
        }
    }

    // Anything left from an upload that was cut off goes first.
    xStreamBufferReset(sb);
    const uint32_t gen_before = d.slots->ota.generation();
    d.slots->ota_upload.publish(core::state::OtaUpload{static_cast<uint32_t>(size)});

    size_t sent = 0;
    while (sent < size) {
        const size_t want = size - sent < kChunk ? size - sent : kChunk;
        const int n = httpd_req_recv(r, reinterpret_cast<char*>(g_buf), want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            // The phone went away. `ota` notices the stream stall and
            // abandons the half-written slot by itself.
            ESP_LOGW(kTag, "upload cut off at %u of %u", static_cast<unsigned>(sent),
                     static_cast<unsigned>(size));
            return ESP_FAIL;
        }
        size_t queued = 0;
        while (queued < static_cast<size_t>(n)) {
            const size_t q = xStreamBufferSend(sb, g_buf + queued, n - queued,
                                               pdMS_TO_TICKS(10000));
            if (q == 0) {
                return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                           "the dash stopped taking the upload");
            }
            queued += q;
        }
        sent += n;
        g_web.touch();
    }

    // Wait for `ota` to come to a verdict on this upload.
    const uint32_t t0 = d.clock->now_ms();
    for (;;) {
        const core::state::Ota o = d.slots->ota.get();
        const bool fresh = d.slots->ota.generation() != gen_before;
        if (fresh && (o.install == core::ota::Install::Done ||
                      o.install == core::ota::Install::Failed)) {
            char body[160];
            if (o.install == core::ota::Install::Done) {
                std::snprintf(body, sizeof body, "{\"ok\":true,\"dev\":\"%s\"}", o.other_ver);
            } else {
                std::snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s%s%s\"}",
                              core::ota::install_error_name(o.error),
                              o.error == core::ota::InstallError::BadImage ? ": " : "",
                              o.error == core::ota::InstallError::BadImage
                                  ? core::ota::image_error_name(o.image_error) : "");
            }
            g_web.touch();
            return send_json(r, body);
        }
        if (d.clock->now_ms() - t0 > kInstallWaitMs) {
            return send_json(r, "{\"ok\":false,\"error\":\"no answer from the installer\"}");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

httpd_handle_t start_server() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 1;
    cfg.task_priority = 3;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    httpd_handle_t h = nullptr;
    if (httpd_start(&h, &cfg) != ESP_OK) return nullptr;

    const httpd_uri_t uris[] = {
        {"/",              HTTP_GET,  page_get,      nullptr},
        {"/api/status",    HTTP_GET,  status_get,    nullptr},
        {"/api/logs",      HTTP_GET,  logs_list_get, nullptr},
        {"/logs/*",        HTTP_GET,  log_get,       nullptr},
        {"/logs/*",        HTTP_DELETE, log_delete,  nullptr},
        {"/api/settings",  HTTP_GET,  settings_get,  nullptr},
        {"/api/set",       HTTP_POST, settings_set,  nullptr},
        {"/fw/stable.bin", HTTP_GET,  fw_get,        nullptr},
        {"/fw/dev.bin",    HTTP_GET,  fw_get,        nullptr},
        {"/fw",            HTTP_POST, fw_post,       nullptr},
        {"/api/games",     HTTP_GET,  games_list_get, nullptr},
        {"/games/*",       HTTP_PUT,  game_put,      nullptr},
        {"/games/*",       HTTP_DELETE, game_delete, nullptr},
    };
    for (const httpd_uri_t& u : uris) httpd_register_uri_handler(h, &u);
    return h;
}

// --- the radio ------------------------------------------------------------

void on_wifi_event(void*, esp_event_base_t, int32_t id, void*) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        g_web.clients.fetch_add(1);
        g_web.touch();
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (g_web.clients.load() > 0) g_web.clients.fetch_sub(1);
    }
}

class Hotspot {
public:
    explicit Hotspot(Deps& d) : d_(d) {}

    void run() {
        uint32_t request_gen = d_.slots->wifi_request.generation();
        uint32_t last_pub_ms = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
            uint32_t now = d_.clock->now_ms();

            const uint32_t gen = d_.slots->wifi_request.generation();
            if (gen != request_gen) {
                request_gen = gen;
                const auto op = d_.slots->wifi_request.get().op;
                if (w_.state == core::wifi::State::On) stop("turned off");
                else if (op == core::state::WifiRequest::Toggle) start();
            }

            if (w_.state != core::wifi::State::On) continue;
            // Read the clock again: start() above stamps last_use later than
            // `now`, and an unsigned now - last would then read as days idle.
            now = d_.clock->now_ms();
            const float mph = d_.slots->motion.get().best_mph;
            const uint32_t last = g_web.last_use_ms.load();
            if (core::wifi::should_stop(now, last, mph)) {
                stop(mph > core::wifi::kMovingMph ? "moving" : "idle");
                continue;
            }
            if (now - last_pub_ms >= 1000) {
                if (now / 5000 != last_pub_ms / 5000) log_ram("on");
                last_pub_ms = now;
                w_.clients = g_web.clients.load();
                w_.off_in_s = static_cast<uint16_t>(core::wifi::off_in_s(now, last));
                publish();
            }
        }
    }

private:
    void publish() { d_.slots->wifi.publish(w_); }

    void start() {
        w_ = core::state::Wifi{};
        w_.state = core::wifi::State::Starting;
        publish();
        log_ram("before start");
        // The health slot, not the tracker: that is keeper's, and this is
        // another task.
        if (!d_.slots->health.get().ok(core::state::Dev::BleHost)) {
            ESP_LOGW(kTag, "no link to the C6, so no Wi-Fi");
            w_.state = core::wifi::State::Failed;
            publish();
            return;
        }

        core::wifi::make_password(esp_random(), esp_random(), w_.pass);
        const esp_err_t up = bring_up();
        if (up != ESP_OK) {
            if (up == ESP_ERR_NOT_SUPPORTED || ++refusals_ >= kRefusalsBeforeC6Update) {
                ESP_LOGW(kTag, "the C6 refuses Wi-Fi: replacing its firmware");
                w_.state = core::wifi::State::UpdatingC6;
                w_.pass[0] = '\0';
                publish();
                c6_firmware_push_now();           // restarts the dash on success
            }
            w_ = core::state::Wifi{};
            w_.state = core::wifi::State::Failed;
            publish();
            return;
        }
        refusals_ = 0;
        g_web.touch();
        g_web.clients.store(0);
        server_ = start_server();
        if (!server_) {
            ESP_LOGE(kTag, "web server did not start");
            esp_wifi_stop();
            w_.state = core::wifi::State::Failed;
            publish();
            return;
        }
        w_.state = core::wifi::State::On;
        w_.off_in_s = static_cast<uint16_t>(core::wifi::kIdleOffMs / 1000u);
        ESP_LOGI(kTag, "hotspot \"%s\" up at 192.168.4.1", core::wifi::kSsid);
        log_ram("up");
        publish();
    }

    esp_err_t bring_up() {
        // The netif once per boot, whatever the C6 says: making the default
        // AP netif a second time asserts inside esp_netif.
        if (!netif_made_) {
            esp_netif_init();
            const esp_err_t ev = esp_event_loop_create_default();
            if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) return ev;
            esp_netif_create_default_wifi_ap();
            esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr);
            netif_made_ = true;
        }
        if (!inited_) {
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            const esp_err_t e = esp_wifi_init(&cfg);
            if (e != ESP_OK) {
                ESP_LOGW(kTag, "esp_wifi_init: %s", esp_err_to_name(e));
                return e;
            }
            inited_ = true;
        }
        wifi_config_t ap = {};
        std::snprintf(reinterpret_cast<char*>(ap.ap.ssid), sizeof ap.ap.ssid, "%s",
                      core::wifi::kSsid);
        ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(core::wifi::kSsid));
        std::snprintf(reinterpret_cast<char*>(ap.ap.password), sizeof ap.ap.password, "%s",
                      w_.pass);
        ap.ap.channel = 6;
        ap.ap.max_connection = 2;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.pmf_cfg.required = false;
        esp_err_t e = esp_wifi_set_mode(WIFI_MODE_AP);
        if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (e == ESP_OK) e = esp_wifi_start();
        // The event loop above, and whatever esp_wifi_init made on the
        // hosted side, come in above ctrl; take them down at once rather
        // than at ble's next once-a-second pass.
        demote_radio_tasks(false);
        if (e != ESP_OK) ESP_LOGW(kTag, "hotspot did not start: %s", esp_err_to_name(e));
        return e;
    }

    void stop(const char* why) {
        if (server_) {
            httpd_stop(server_);
            server_ = nullptr;
        }
        esp_wifi_stop();
        ESP_LOGI(kTag, "hotspot off (%s)", why);
        log_ram("off");
        w_ = core::state::Wifi{};
        publish();
    }

    Deps& d_;
    core::state::Wifi w_;
    httpd_handle_t server_ = nullptr;
    bool netif_made_ = false;
    bool inited_ = false;
    int refusals_ = 0;
};

void wifi_task(void* arg) {
    Deps& d = *static_cast<Deps*>(arg);
    g_web.d = &d;
    static Hotspot hotspot(d);
    hotspot.run();
}

}  // namespace

void start_wifi(Deps& d) {
    d.slots->wifi.publish(core::state::Wifi{});
    xTaskCreatePinnedToCore(wifi_task, "wifi", 4096, &d, 3, nullptr, 1);
}

}  // namespace services
