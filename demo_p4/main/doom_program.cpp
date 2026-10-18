// See doom_program.hpp. The second half of this file is doomgeneric's
// platform layer: six plain C functions with no context argument, so the
// running program is the one file-static pointer below, set once by start().
#include "doom_program.hpp"
#include "p4_panel.hpp"
#include "drivers/sdcard.hpp"
#include "core/wifi/wifi.hpp"
#include "ui/framebuffer_canvas.hpp"
#include "ui/geometry.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

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

namespace demo {
namespace {

constexpr const char* kTag = "doom";
constexpr uint32_t kStateRefreshMs = 2000;

// Doom's 320x200 at 2x is 640x400: the left of the 800x480 view, centred
// top to bottom. The right 160 px is the button legend, over the touch zones.
constexpr int kScale = 2;
constexpr int kPicX = 0;
constexpr int kPicY = (480 - DOOMGENERIC_RESY * kScale) / 2;   // 40
constexpr int kLegendX = DOOMGENERIC_RESX * kScale;            // 640
constexpr int kLegendW = 800 - kLegendX;                       // 160
constexpr int kLegendH = 480;
constexpr int kBandH = kLegendH / 5;                           // touch_buttons' bands

// 32 kB of stack, in PSRAM: the renderer recurses down the BSP and the
// internal RAM is the radio's. Pinned where render was, at its priority.
constexpr uint32_t kStackBytes = 32 * 1024;
constexpr UBaseType_t kPriority = 6;
constexpr BaseType_t kCore = 0;

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

DoomProgram* g_running = nullptr;   // doomgeneric's hooks have no context

void doom_task(void* arg) {
    auto* prog = static_cast<DoomProgram*>(arg);
    ESP_LOGI(kTag, "starting on %s", prog->wad_path());
    // doomgeneric keeps argv; it lives as long as this task, which is forever.
    static char a0[] = "doom";
    static char a1[] = "-iwad";
    static char* argv[] = {a0, a1, const_cast<char*>(prog->wad_path()), nullptr};
    doomgeneric_Create(3, argv);
    for (;;) doomgeneric_Tick();
}

// The legend down the right edge, one band per button, drawn once.
void draw_legend(P4Panel& panel) {
    auto* buf = static_cast<ui::Color*>(heap_caps_malloc(
        static_cast<size_t>(kLegendW) * kLegendH * sizeof(ui::Color),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) return;
    ui::FrameBufferCanvas fb(buf, kLegendW, kLegendH);
    ui::Canvas& c = fb;   // the base declares draw_text's defaults
    c.fill(ui::color::kBlack);
    struct Band { const char* key; const char* top; const char* sub; };
    constexpr Band kBands[5] = {
        {"A", "FWD",   "A+B MENU"},
        {"B", "BACK",  "A+B MENU"},
        {"C", "LEFT",  "C+D USE"},
        {"D", "RIGHT", "C+D USE"},
        {"E", "FIRE",  "OK / YES"},
    };
    for (int i = 0; i < 5; ++i) {
        const int16_t y = static_cast<int16_t>(i * kBandH);
        if (i) c.fill_rect(ui::Rect{0, y, kLegendW, 1}, ui::color::kDim);
        c.draw_text(12, static_cast<int16_t>(y + 14), kBands[i].key, ui::color::kAccent, 3);
        c.draw_text(44, static_cast<int16_t>(y + 14), kBands[i].top, ui::color::kText, 3);
        c.draw_text(12, static_cast<int16_t>(y + 58), kBands[i].sub, ui::color::kDim, 2);
    }
    panel.blit_view(kLegendX, 0, kLegendW, kLegendH, buf, 1);
    heap_caps_free(buf);
}

}  // namespace

bool DoomProgram::find_wad() const {
    WadPick pick;
    if (!card_.present() || !hal::ok(card_.list("games", consider_wad, &pick)) ||
        !pick.name[0]) {
        return false;
    }
    // The VFS path, for Doom's own fopen: drivers::SdCard mounts at /sd.
    std::snprintf(wad_path_, sizeof wad_path_, "/sd/games/%s", pick.name);
    return true;
}

const char* DoomProgram::state() const {
    const uint32_t now = now_ms();
    if (!state_known_ || now - state_ms_ >= kStateRefreshMs) {
        state_known_ = true;
        state_ms_ = now;
        state_ = !card_.present() ? "no card" : find_wad() ? "ready" : "no WAD";
    }
    return state_;
}

bool DoomProgram::launch() {
    if (requested_.load()) return true;
    if (!find_wad()) {
        state_known_ = false;   // say why on the next frame, not in 2 s
        return false;
    }
    requested_.store(true);
    return true;
}

void DoomProgram::start() {
    if (g_running) return;
    g_running = this;
    if (xTaskCreatePinnedToCoreWithCaps(doom_task, "doom", kStackBytes, this, kPriority,
                                        nullptr, kCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "no memory for the Doom task");
    }
}

}  // namespace demo

// --- doomgeneric's platform layer ------------------------------------------

namespace {

using demo::g_running;

// Doom's 8-bit frame as RGB565 for blit_view, and its palette in the same.
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
    const uint8_t m = g_running->buttons().mask.load();
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
    auto& panel = g_running->panel();
    // Black everywhere Doom does not draw, then the legend.
    static uint16_t black[800] = {};
    for (int y = 0; y < 480; ++y) panel.blit_view(0, y, 800, 1, black, 1);
    demo::draw_legend(panel);
    g_frame565 = static_cast<uint16_t*>(heap_caps_malloc(
        DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // Always run: the old "joyb_speed 31" trick chocolate-doom keeps. A
    // saved default.cfg on the card overrides it.
    joybspeed = 31;
}

void DG_DrawFrame() {
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
        g_running->panel().blit_view(demo::kPicX, demo::kPicY, DOOMGENERIC_RESX,
                                     DOOMGENERIC_RESY, g_frame565, demo::kScale);
    }
    // A tick for everything else on this core (and its idle task, which the
    // watchdog watches): Doom only sleeps when it is ahead of 35 fps.
    vTaskDelay(1);
}

void DG_SleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

uint32_t DG_GetTicksMs() { return demo::now_ms(); }

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

}  // extern "C"
