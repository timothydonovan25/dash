// The concrete drivers. One class per chip, each implementing one hal
// interface and knowing nothing about the application.
//
// ────────────────────────────────────────────────────────────────────────
//  MOST OF THIS IS UNPROVEN. Every class below was written against a
//  datasheet and a netlist, which is not the same as working; what has run
//  on the first board (2026-10-28) is in docs/bringup.md's status table.
//  CLAUDE.md says not to write drivers you cannot test, and the spirit of
//  that is respected here: these are the shape of the drivers, the register
//  maps, the init orders and the health reporting. The parts that can only
//  be settled on a bench are marked BRINGUP and left honest rather than
//  guessed at confidently.
//
//  Work down DIAG → Sensors. That screen exists for bring-up.
// ────────────────────────────────────────────────────────────────────────
#pragma once
#include "drivers/clock.hpp"
#include "drivers/pod.hpp"
#include "drivers/ili9488.hpp"
#include "drivers/icm42688p.hpp"
#include "drivers/pa1616d.hpp"
#include "drivers/engine_sense.hpp"
#include "drivers/nvs_storage.hpp"
#include "drivers/sdcard.hpp"
#include "drivers/pca9955b.hpp"
#include "drivers/vin_sense.hpp"
