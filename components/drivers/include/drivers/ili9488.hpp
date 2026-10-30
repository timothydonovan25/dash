// ILI9488 on the 8-bit 8080 bus, plus the backlight.
//
// Three things about this panel and this bus drive the design:
//
//  - **RGB565 goes out as is**, two bytes per pixel. 8080 mode accepts it;
//    only SPI mode needed the RGB666 conversion. The I80 peripheral swaps
//    each pixel's bytes on the way out, because the panel wants the high
//    byte first and the UI's pixels are little-endian in memory.
//  - **The P4's DMA wants every transfer's address and length on a 32-byte
//    boundary.** The UI blits rectangles of any size, so pixels are staged
//    through an aligned buffer owned here, and any tail shorter than 32
//    bytes goes out as the parameters of a Memory Write Continue command.
//    The caller's buffer is free the moment blit() returns.
//  - **The backlight is on the PCA9955B**, so set_backlight() forwards there.
//  - **Two panels fit FPC1, with WR and RS on opposite pins**, and RD is
//    tied high, so neither can be read. The first init() tells them apart
//    by the one line that comes back, TE: each wiring is tried in turn,
//    starting with the caller's guess, and the panel's tearing-effect
//    output only pulses once its init went in the right way round.
//
// The panel command set comes from the `esp_lcd_ili9488` managed component,
// except the UEED035HV's init, which is VIEWE's own ST7365 table.
// This class bridges that to hal::IPanel and owns the blit path itself.
#pragma once
#include "hal/i_display.hpp"
#include <cstdint>

namespace drivers {

class Pca9955b;

// The panels FPC1 takes. The values are stored (hal::kKeyPanelModel), so
// they never change.
enum class PanelModel : uint8_t {
    Ht035   = 0,   // HT035SHV007NS, ILI9488, IPS
    Ueed035 = 1,   // VIEWE UEED035HV-RX40-L001A, ST7365, transflective
};

class Ili9488 final : public hal::IPanel {
public:
    // `lcd_bus` is bsp::BusStatus::lcd_bus. Either may be null, and the
    // panel then reports NotPresent rather than crashing.
    Ili9488(void* lcd_bus, Pca9955b* backlight)
        : bus_(lcd_bus), backlight_(backlight) {}

    hal::Status init() override;
    uint16_t width() const override;
    uint16_t height() const override;
    hal::Status blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t* pixels) override;
    hal::Status set_backlight(uint8_t percent) override;

    // Which wiring the first init() tries first. Call before it.
    void set_first_guess(PanelModel m) { model_ = m; }
    // The panel init() settled on: the one whose TE answered, or the guess
    // if neither did (no TE, or no panel), when confirmed() is false.
    PanelModel model() const { return model_; }
    bool confirmed() const { return confirmed_; }

private:
    hal::Status wire_for(PanelModel m);
    hal::Status start();
    bool te_answers();

    void*     bus_;
    Pca9955b* backlight_;
    void*     io_ = nullptr;        // esp_lcd_panel_io_handle_t
    void*     panel_ = nullptr;     // esp_lcd_panel_handle_t
    uint8_t*  staging_ = nullptr;   // DMA-capable, 64-byte aligned
    PanelModel model_ = PanelModel::Ht035;
    bool      swapped_ = false;     // bus_ has WR and DC traded (bsp)
    bool      probed_ = false;
    bool      confirmed_ = false;
};

}  // namespace drivers
