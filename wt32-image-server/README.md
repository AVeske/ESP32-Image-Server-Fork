# wt32-image-server

The **WT32-SC01 (V3.2)** build of [ESP32-Image-Server](../README.md) — a 4-slot
320×480 image kiosk with a 2×2 touch grid. The HTTP API, data overlay,
naming/discovery and serial console are documented in the
[root README](../README.md); this page covers what is specific to this board.

- **MCU/display:** classic ESP32 (WROVER + PSRAM), 3.5″ 320×480 ST7796 SPI,
  FT5x06 capacitive touch
- **Framework:** Arduino + [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- **Frame:** native full-screen 320×480 RGB565 = 307 200 B per slot, no upscaling

> **Status: builds clean (`pio run`), not yet verified on hardware.** The panel
> init, touch hotspot mapping and cross-device GETs want a real WT32-SC01 to
> confirm.

## Build & flash

```sh
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # serial @ 115200 (UART0)
```

## Memory layout

The 300 KB frame buffer does not fit in internal DRAM, so it is allocated in
**PSRAM** (`ps_malloc`) — `BOARD_HAS_PSRAM` is set in
[`platformio.ini`](platformio.ini). Without PSRAM the firmware still boots and
serves, but frame pushes are dropped with a message on serial.

The four raw slots are ~1.2 MB together, which needs more data partition than the
stock layout gives: [`partitions.csv`](partitions.csv) is a custom **no-OTA**
table with a large LittleFS partition.

## Touch hotspots

Each slot carries an invisible 2×2 grid of touch cells (top-left, top-right,
bottom-left, bottom-right), one action per cell — see
[Slots and actions](../README.md#slots-and-actions) for the value syntax. Cells
that share the **same** value flash together on tap, so two or more quadrants
pointing at one endpoint read as a single larger button. Empty cells do nothing.
Actions are stored as 16 lines (`slot*4 + cell`) in `/buttons.txt`.

## Panel config

The pins, driver and dimensions are all in
[`include/lgfx_wt32.h`](include/lgfx_wt32.h), taken from the
[reference project](https://github.com/luckyluckhcccp/wt32-sc01-v3.2-LVGL8-lovyan-gfx).

If your unit is a **different panel** (e.g. an 800×480 5″ RGB), swap
`_panel_instance` for the matching `lgfx::Panel_*` and update `PANEL_W` /
`PANEL_H` and the pins there — nothing outside that header needs to move, since
the frame size is derived from `PANEL_W`/`PANEL_H`.

## Battery

`/state` reports `battery: null` — the WT32-SC01 is USB-powered and exposes no
battery divider by default. If you wire a 2:1 divider to an ADC pin, set
`BAT_ADC_PIN` at the top of [`src/main.cpp`](src/main.cpp) to enable the reading.

## Notes

- RGB565 is packed **big-endian** by the framer page; the firmware types the
  source as `lgfx::swap565_t` so LovyanGFX converts it to the panel's native byte
  order. Reading it as a plain `uint16` rotates the channels.
- `GET /frame?slot=N` streams a slot straight from flash rather than through the
  single PSRAM buffer, so any slot can be read back without disturbing what is on
  the display.
