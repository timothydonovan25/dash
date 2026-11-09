# VIEWE UEED035HV (ST7365) — usage

3.5" **transflective** (sunlight-readable) panel, Sitronix ST7365, same
40-pin FPC footprint as the HT035. Spec V1.0 (`UEED035HV.pdf`). Ordered
2026-10-30 as the sunlight upgrade.

## Differences from the HT035
- **WR and RS swapped**: WR on FPC 10, RS (D/C) on FPC 11 — Sitronix puts
  SPI's SCL on WRX, Ilitek on D/CX. Firmware swaps GPIO51/52 roles, and R30's
  22 Ω then sits on DC instead of WR.
- **Init is VIEWE's ST7365 table**, byte for byte (minus MADCTL, which
  `swap_xy`/`mirror` send). The ILI9488 handle is used only for reset, swap
  and mirror. This is the one command set we write by hand.
- Write cycle is "TBD" in the spec: starts at **10 MHz**
  (`kTftWrHzUeed035`); raise it as the HT035's was.
- Came up mirrored left-to-right with both mirrors, so it uses `swap_xy` +
  one mirror (2026-11-06).

## Detection
`drivers::Ili9488` sends TEON both ways round and listens on TE (GPIO49) for
~3 frames; whichever wiring answers is the fitted panel. Neither → falls
back to a guess so a panel with only TE broken still shows a picture.

## Status
Probe not yet run on the bike board (bringup status row 3).
