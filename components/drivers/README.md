One component per chip. A driver knows its chip and nothing about the app.

`clock` `mcp23s17` `pod` (MCP23017) `engine_sense` `pa1616d` `icm42688p`
`ili9488` `nvs_storage` `sdcard`. The ILI9488 command set comes from the
`esp_lcd_ili9488` managed component rather than being written here.

These compile clean for esp32s3 against IDF v6.0.2. **None of it has run.**
The PCB is at fab, and compiling is not working: they are written against
the datasheets and the netlist, which is not the same as correct — every class
is the right shape, the register maps and init orders are there, and the
places that can only be settled on a bench are marked BRINGUP rather than
guessed at confidently. In particular:

- the IMU axis mapping has never been checked against a real part;
- the tach refractory window is a starting number, not a measured one;
- I2C bus recovery does not yet clock a stuck slave off the bus.

`tach_capture` and `hall_capture` from the architecture sketch are one
`engine_sense` here — they are the same three edge counters behind one
latched read. `backlight` is part of `ili9488`, being one LEDC channel.

When the board arrives, work down DIAG → Sensors. That screen exists for
this week.
