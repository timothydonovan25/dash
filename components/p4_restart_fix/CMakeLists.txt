idf_component_register(
    SRCS "restart_fix.c"
    PRIV_REQUIRES esp_system soc)

# Keeps the ESP32-P4 v1.x SHA clock on so any reset can boot again; see
# restart_fix.c. Every restart -- esp_restart(), a panic, the watchdog --
# ends in esp_restart_noos(); startup gates the clock in
# esp_perip_clk_init(); and IDF's SHA driver turns its clock off
# through esp_crypto_sha_enable_periph_clk().
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_restart_noos"
    "-Wl,--wrap=esp_perip_clk_init"
    "-Wl,--wrap=esp_crypto_sha_enable_periph_clk")
