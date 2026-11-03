// See doom.hpp. The second half of this file is doomgeneric's platform
// layer: plain C functions with no context argument, so the running program
// is the one file-static pointer below, set once by launch().
#include "doom/doom.hpp"
#include "core/wifi/wifi.hpp"
#include "ui/geometry.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

extern "C" {
#include "doomgeneric.h"
#include "doomkeys.h"

// From i_video.c and m_controls.c. Not their headers: doomtype.h makes
// `boolean` an enum in C and an unsigned int in C++, and both are 4 bytes.
struct color { uint32_t b : 8; uint32_t g : 8; uint32_t r : 8; uint32_t a : 8; };
extern struct color colors[256];
extern unsigned int palette_changed;
extern int joybspeed;
}

static_assert(doom::kWidth == DOOMGENERIC_RESX && doom::kHeight == DOOMGENERIC_RESY,
              "doom::Board::frame() is Doom's own picture");

namespace doom {
namespace {

constexpr const char* kTag = "doom";
constexpr uint32_t kCardStateRefreshMs = 2000;

// 32 kB of stack, in PSRAM: the renderer recurses down the BSP and the
// internal RAM is the radio's.
constexpr uint32_t kStackBytes = 32 * 1024;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// The preferred names first, in case the card has more than one; then any
// .wad at all.
constexpr const char* kKnown[] = {"doom2.wad", "doom.wad", "doom1.wad",
                                  "freedoom2.wad", "freedoom1.wad"};

struct WadPick {
    char name[40] = {};
    int  rank = 1000;
};

void consider_wad(const char* name, size_t size, void* ctx) {
    auto* pick = static_cast<WadPick*>(ctx);
    if (size == 0 || !core::wifi::valid_game_name(name)) return;
    int rank = 100;
    for (int i = 0; i < static_cast<int>(sizeof kKnown / sizeof kKnown[0]); ++i) {
        if (strcasecmp(name, kKnown[i]) == 0) rank = i;
    }
    if (rank < pick->rank) {
        pick->rank = rank;
        std::snprintf(pick->name, sizeof pick->name, "%s", name);
    }
}

Program* g_running = nullptr;   // doomgeneric's hooks have no context

void doom_task(void* arg) {
    auto* prog = static_cast<Program*>(arg);
    ESP_LOGI(kTag, "starting on %s", prog->wad_path());
    // doomgeneric keeps argv; it lives as long as this task, which is forever.
    static char a0[] = "doom";
    static char a1[] = "-iwad";
    static char* argv[] = {a0, a1, const_cast<char*>(prog->wad_path()), nullptr};
    doomgeneric_Create(3, argv);
    for (;;) doomgeneric_Tick();
}

}  // namespace

bool Program::find_wad() const {
    WadPick pick;
    if (!card_.present() || !hal::ok(card_.list("games", consider_wad, &pick)) ||
        !pick.name[0]) {
        return false;
    }
    // The VFS path, for Doom's own fopen: drivers::SdCard mounts at /sd.
    std::snprintf(wad_path_, sizeof wad_path_, "/sd/games/%s", pick.name);
    return true;
}

const char* Program::state() const {
    if (dead_.load()) return "error";
    if (!board_.parked()) return "parked only";
    if (task_) return "paused";
    const uint32_t now = now_ms();
    if (!card_state_known_ || now - card_state_ms_ >= kCardStateRefreshMs) {
        card_state_known_ = true;
        card_state_ms_ = now;
        card_state_ = !card_.present() ? "no card" : find_wad() ? "ready" : "no WAD";
    }
    return card_state_;
}

bool Program::launch() {
    if (dead_.load() || !board_.parked()) return false;
    if (active_.load()) return true;
    if (task_) {
        ESP_LOGI(kTag, "resuming");
        active_.store(true);
        xTaskNotifyGive(static_cast<TaskHandle_t>(task_));
        return true;
    }
    if (!find_wad()) {
        card_state_known_ = false;   // say why on the next frame, not in 2 s
        return false;
    }
    g_running = this;
    active_.store(true);
    TaskHandle_t h = nullptr;
    if (xTaskCreatePinnedToCoreWithCaps(doom_task, "doom", kStackBytes, this, priority_, &h,
                                        core_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "no memory for the Doom task");
        active_.store(false);
        g_running = nullptr;
        return false;
    }
    task_ = h;
    return true;
}

void Program::pause() {
    ESP_LOGI(kTag, "paused: the panel is the dash's");
    active_.store(false);
    // A launch() between the store and here is a notification already
    // counted, so this returns at once.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    board_.begin();
}

void Program::stop() {
    ESP_LOGE(kTag, "Doom stopped; DIAG -> Doom is off until the next power-on");
    dead_.store(true);
    active_.store(false);
    for (;;) vTaskSuspend(nullptr);
}

}  // namespace doom

// --- doomgeneric's platform layer ------------------------------------------

namespace {

using doom::g_running;

// Doom's 8-bit frame as RGB565, and its palette in the same.
uint16_t* g_frame565 = nullptr;
uint16_t  g_pal565[256] = {};

// Key events for DG_GetKey: what each held key was last reported as, and a
// small queue of changes not yet handed over.
constexpr int kKeyCount = 8;
constexpr unsigned char kKeys[kKeyCount] = {
    KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW,
    KEY_FIRE, KEY_ENTER, KEY_ESCAPE, KEY_USE};
bool g_down[kKeyCount] = {};
// A key also sent with E: 'y' answers Doom's yes/no questions.
bool g_y_down = false;

struct KeyEvent { unsigned char key; bool pressed; };
KeyEvent g_queue[16];
int g_q_head = 0, g_q_len = 0;

void push(unsigned char key, bool pressed) {
    if (g_q_len == static_cast<int>(sizeof g_queue / sizeof g_queue[0])) return;
    g_queue[(g_q_head + g_q_len++) % (sizeof g_queue / sizeof g_queue[0])] = {key, pressed};
}

// The held buttons as the keys they stand for, chords first: A+B is Escape
// rather than forward and back at once, C+D is use rather than both turns.
void sample_buttons() {
    const uint8_t m = g_running->board().held();
    const bool a = m & 1, b = m & 2, c = m & 4, d = m & 8, e = m & 16;
    const bool menu = a && b, use = c && d;
    const bool want[kKeyCount] = {
        a && !menu, b && !menu, c && !use, d && !use, e, e, menu, use};
    for (int i = 0; i < kKeyCount; ++i) {
        if (want[i] != g_down[i]) {
            g_down[i] = want[i];
            push(kKeys[i], want[i]);
        }
    }
    if (e != g_y_down) {
        g_y_down = e;
        push('y', e);
    }
}

}  // namespace

extern "C" {

void DG_Init() {
    g_running->board().begin();
    g_frame565 = static_cast<uint16_t*>(heap_caps_malloc(
        DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // Always run: the old "joyb_speed 31" trick chocolate-doom keeps. A
    // saved default.cfg on the card overrides it.
    joybspeed = 31;
}

void DG_DrawFrame() {
    // Checked here, between pictures, so nothing of Doom's is mid-write when
    // the dash takes the panel back.
    if (!g_running->board().parked()) g_running->pause();
    if (palette_changed) {
        palette_changed = 0;
        for (int i = 0; i < 256; ++i) {
            g_pal565[i] = ui::rgb(static_cast<uint8_t>(colors[i].r),
                                  static_cast<uint8_t>(colors[i].g),
                                  static_cast<uint8_t>(colors[i].b));
        }
    }
    if (g_frame565) {
        const pixel_t* in = DG_ScreenBuffer;
        for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; ++i) {
            g_frame565[i] = g_pal565[in[i]];
        }
        g_running->board().frame(g_frame565);
    }
    // A tick for everything else on this core (and its idle task, which the
    // watchdog watches): Doom only sleeps when it is ahead of 35 fps.
    vTaskDelay(1);
}

void DG_SleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

uint32_t DG_GetTicksMs() { return doom::now_ms(); }

int DG_GetKey(int* pressed, unsigned char* key) {
    if (g_q_len == 0) sample_buttons();
    if (g_q_len == 0) return 0;
    const KeyEvent ev = g_queue[g_q_head];
    g_q_head = (g_q_head + 1) % static_cast<int>(sizeof g_queue / sizeof g_queue[0]);
    --g_q_len;
    *pressed = ev.pressed ? 1 : 0;
    *key = ev.key;
    return 1;
}

void DG_SetWindowTitle(const char* title) { ESP_LOGI("doom", "%s", title); }

void DG_Quit(void) { g_running->pause(); }

void DG_Exit(int status) {
    ESP_LOGE("doom", "exit(%d)", status);
    g_running->stop();
}

}  // extern "C"
