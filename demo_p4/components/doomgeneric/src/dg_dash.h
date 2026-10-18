// dash: included before every doomgeneric source (CMakeLists.txt, -include).
//
// DG_EXTRAM puts a zeroed global in PSRAM, as IDF's EXT_RAM_BSS_ATTR does:
// the default mapping places .ext_ram.bss.* there and IDF zeroes it at boot.
// On the renderer's eleven big arrays (~205 kB of Doom's 244 kB of .bss),
// which would otherwise sit in the internal RAM the SDIO link, lwIP and
// NimBLE share. (A linker-fragment `* (extram_bss)` mapping would be the
// tidy way; IDF v6.0.2's ldgen emits nothing for it on this target, for
// lwIP's own mapping as much as for this one.)
#pragma once
#define DG_EXTRAM __attribute__((section(".ext_ram.bss.doom")))
