// The concrete drivers. One class per chip, each implementing one hal
// interface and knowing nothing about the application.
//
// ────────────────────────────────────────────────────────────────────────
//  NONE OF THIS HAS RUN. The PCB is at fab. Every class below is written
//  against a datasheet and a netlist, which is not the same as working.
//  CLAUDE.md says not to write drivers you cannot test, and the spirit of
//  that is respected here: these are the shape of the drivers, the register
//  maps, the init orders and the health reporting. The parts that can only
//  be settled on a bench are marked BRINGUP and left honest rather than
//  guessed at confidently.
//
//  When the board arrives, work down DIAG → Sensors. That screen exists for
//  this week.
// ────────────────────────────────────────────────────────────────────────
#pragma once
#include "drivers/clock.hpp"
#include "drivers/mcp23s17.hpp"
#include "drivers/pod.hpp"
#include "drivers/ili9488.hpp"
#include "drivers/icm42688p.hpp"
#include "drivers/pa1616d.hpp"
#include "drivers/engine_sense.hpp"
#include "drivers/nvs_storage.hpp"
#include "drivers/sdcard.hpp"
