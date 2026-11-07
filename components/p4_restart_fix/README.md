Keeps the ESP32-P4's SHA engine clocked and out of reset across every
restart, because a v1.x P4 whose reset finds it gated cannot boot until a
power cycle (the ROM's SHA write faults; seen on the demo board, v1.3,
and on the bike's v3.2 on 2026-11-06).
IDF gates it at startup, after core-dump verification, after every SHA use
and around a panic's core-dump write, which hashes with the ROM's own
`ets_sha_*` calls (wrapped since 2026-11-06: the bike wedged after a reset
that left an erased dump partition). Both boards link it; the reasoning is
in `restart_fix.c`. Cost: one peripheral clock.
