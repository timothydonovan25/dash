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

// Doom's exit() calls (I_Error's above all: a bad WAD, a card pulled
// mid-read) would end the whole firmware. Here they end only Doom: the
// dash's DG_Exit hands the panel back and stops Doom's task for good.
// I_Quit's pause, as doomgeneric.h declares it for the port.
void DG_Quit(void);
void DG_Exit(int status) __attribute__((noreturn));
#define exit DG_Exit
