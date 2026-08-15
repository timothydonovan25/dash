// Entry point. Everything real is wired here and nowhere else.
//
// Boot order is NOT arbitrary — see docs/system-architecture.md section 6.
// The display's reset line hangs off the MCP23S17, so the expander comes
// up first.
#include "bsp/pins.hpp"
#include "core/nav/nav.hpp"

extern "C" void app_main() {
    // 1. bsp::init_buses()      SPI2, SPI3, I2C0, UART1, LEDC
    // 2. MCP23S17               directions, lamps off, TFT_RST pulse
    // 3. ILI9488                panel init, backlight to a low duty
    // 4. boot screen            immediately, before any sensor
    // 5. IMU, GPS, SD, pod      each reporting into health
    // 6. start tasks            see section 5 for cores and priorities
    //
    // Nothing here yet. Build the core/ modules and the UI against a fake
    // Frame first — the board has not arrived.
}
