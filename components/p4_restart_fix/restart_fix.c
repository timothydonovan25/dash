// ESP32-P4 v1.x: a reset that finds the SHA engine clock-gated or held in
// reset leaves the chip unable to boot, and only a power cycle clears it.
//
// Found on the demo board (rev v1.3, ROM esp32p4-eco2-20240710). The ROM
// hashes the bootloader as it loads it, and its first SHA register write
// faulted (Store/AMO access fault, MTVAL 0x50091000 = DR_REG_SHA_BASE). The
// HP_SYS_CLKRST gates and resets survive every reset short of a power
// cycle, so the fault repeated through the watchdog resets that followed --
// a dark dash, until the key is cycled or the RTC watchdog eventually digs
// deep enough.
//
// What leaves the engine that way, all of it in IDF:
//   - startup gates the crypto clocks off as unused (esp_perip_clk_init);
//   - with a core dump in flash, startup verifies it with the SHA engine and
//     leaves it gated and held in reset -- this was the one that bit, and
//     CONFIG_ESP_COREDUMP_CHECK_BOOT=n removes it from the boot path;
//   - a panic writes the core dump with SHA immediately before restarting;
//   - the SHA driver gates the clock again after every use;
//   - IDF's own restart path undoes this only when
//     CONFIG_ESP32P4_REV_MIN_FULL == 0 (esp_system/port/soc/esp32p4/
//     system_internal.c), and this project builds for v1.0 and up.
//
// So the engine is handed back to the ROM's terms -- clocked, out of reset
// -- the moment startup gates it, whenever the driver would gate it, and on
// every way into a restart. The cost is one peripheral's clock.
#include <stdbool.h>
#include "esp_attr.h"
#include "soc/hp_sys_clkrst_reg.h"
#include "soc/soc.h"

static inline void IRAM_ATTR sha_ready_for_rom(void)
{
    SET_PERI_REG_MASK(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_CRYPTO_SYS_CLK_EN);
    SET_PERI_REG_MASK(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG,
                      HP_SYS_CLKRST_REG_CRYPTO_SHA_CLK_EN |
                      HP_SYS_CLKRST_REG_CRYPTO_RSA_CLK_EN |
                      HP_SYS_CLKRST_REG_CRYPTO_ECDSA_CLK_EN);
    // Out of reset, the parent crypto block included: sha_ll_reset_register()
    // notes SHA stays held while any of these is.
    CLEAR_PERI_REG_MASK(HP_SYS_CLKRST_HP_RST_EN2_REG,
                        HP_SYS_CLKRST_REG_RST_EN_SHA |
                        HP_SYS_CLKRST_REG_RST_EN_CRYPTO |
                        HP_SYS_CLKRST_REG_RST_EN_DS |
                        HP_SYS_CLKRST_REG_RST_EN_HMAC |
                        HP_SYS_CLKRST_REG_RST_EN_ECDSA);
}

void __real_esp_perip_clk_init(void);

void __wrap_esp_perip_clk_init(void)
{
    __real_esp_perip_clk_init();
    sha_ready_for_rom();
}

void __real_esp_crypto_sha_enable_periph_clk(bool enable);

void __wrap_esp_crypto_sha_enable_periph_clk(bool enable)
{
    // Enabling also resets the engine, which the driver relies on. Disabling
    // is what strands the ROM, so it does not happen.
    if (enable) __real_esp_crypto_sha_enable_periph_clk(true);
}

void __real_esp_restart_noos(void);

// esp_restart(), a panic (after the core dump is written) and the task
// watchdog all end here.
void IRAM_ATTR __wrap_esp_restart_noos(void)
{
    sha_ready_for_rom();
    __real_esp_restart_noos();
}

// A panic's core dump hashes itself with the ROM's own SHA calls, past the
// driver wrap above, and ets_sha_disable() strands the ROM the same way.
// The bike board (v3.2) wedged after a panic on 2026-11-06 with an erased
// dump partition: the reset came mid-dump (the panic's 10 s RTC watchdog,
// or a fault in the dump), never reaching esp_restart_noos(). So the engine
// is made ready before the dump starts, and the ROM's disable is a no-op.
struct panic_info_t;
void __real_esp_core_dump_write(struct panic_info_t *info);

void IRAM_ATTR __wrap_esp_core_dump_write(struct panic_info_t *info)
{
    sha_ready_for_rom();
    __real_esp_core_dump_write(info);
    sha_ready_for_rom();
}

void IRAM_ATTR __wrap_ets_sha_disable(void)
{
    sha_ready_for_rom();
}
