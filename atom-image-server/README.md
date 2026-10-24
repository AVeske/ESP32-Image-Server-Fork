# atom-image-server

The **M5Stack AtomS3R** build of [ESP32-Image-Server](../README.md) — a 4-slot
128×128 image kiosk with single-button gestures. The HTTP API, data overlay,
naming/discovery and serial console are documented in the
[root README](../README.md); this page covers what is specific to this board.

- **MCU/display:** ESP32-S3 (8 MB flash), 0.85″ 128×128 IPS LCD, one button
- **Framework:** Arduino + [M5Unified](https://github.com/m5stack/M5Unified) (M5GFX)
- **Frame:** 128×128 RGB565 = 32 768 B per slot, held in internal DRAM

## Build & flash

```sh
pio run                 # build
pio run -t upload       # flash over USB-C
pio device monitor      # serial @ 115200 (native USB-CDC)
```

The AtomS3R has no dedicated PlatformIO board id; we target `m5stack-atoms3`
(same ESP32-S3 and flash size) and M5Unified auto-detects the actual panel at
runtime.

## Button gestures

The AtomS3R has one button, so each slot maps three gestures to actions:

| gesture | timing |
|---------|--------|
| short click | release **< 0.5 s** |
| long click | held **> 0.7 s** |
| double click | two short clicks within **1.5 s** |

A release between 0.5 s and 0.7 s falls in neither band and is ignored. A single
short fires only after the 1.5 s double-click window passes, so it can still
become a double. An **unconfigured long press** shows the WiFi/IP status on the
LCD. Actions are stored as 12 lines (`slot*3 + gesture`) in `/buttons.txt`;
the value syntax is in the [root README](../README.md#slots-and-actions).

Timings are constants at the top of [`src/main.cpp`](src/main.cpp)
(`SHORT_MAX_MS` / `LONG_MIN_MS` / `DOUBLE_MS`).

## Battery

`/state` reports pack voltage and a rough percentage. The AtomS3R reads the pack
through a 2:1 divider on **GPIO 8** (the older Atom series uses GPIO 33);
`analogReadMilliVolts()` already applies the eFuse calibration, so the firmware
just doubles it back. 3.3 V → 0 %, 4.2 V → 100 % (1S LiPo).

## Marker id

`POST /frame?mid=<n>` records an optional tag id alongside the image in a slot,
reported back as `markerId` in `/state` (`-1` when unset). It exists for
marker-tracking hosts that display an ArUco/AprilTag and need to know which one
is on screen; leave it out if you don't need it.

[`tools/send_aruco.py`](tools/send_aruco.py) generates and pushes an ArUco
marker; [`tools/atomctl.py`](tools/atomctl.py) drives the serial console from the
host.

## Config

SoftAP credentials are at the top of [`src/main.cpp`](src/main.cpp):

```cpp
static const char* AP_SSID = "AtomFramer";
static const char* AP_PASS = "atomframer";   // >= 8 chars, or "" for an open AP
```

## Notes

- RGB565 is packed **big-endian** by the framer page; the firmware types the
  source as `m5gfx::swap565_t` so M5GFX converts it to the panel's native byte
  order. Reading it as a plain `uint16` rotates the channels.
- The framer page can `EXPORT .DAT` (raw 32 768-byte RGB565) or `SAVE PNG + DATA`
  (a normal PNG with the raw RGB565 in a private `daTa` chunk) and load either
  back in later.

## Post-office PNG jobs

The Flask page uploads and stores PNG images and sends this poll response:

```json
{"msg":17,"job":"show_image","arg":"<64-character image id>","image_url":"/api/images/<id>/png"}
```

The Atom downloads the PNG from the configured SERVER_URL with X-API-Key,
retries download failures up to three times, and decodes the 128x128 PNG on
the main loop. It reports done only after display, or error on failure.
Final results use the existing retry queue if the public server is unavailable.
Downloads are limited to 64 KiB and same-server image paths; redirects are disabled.
Local image slots are preserved; the remote image itself is not saved across reboot.
Button letter selection and station letter forwarding remain available.

TLS currently follows the existing poll/result implementation (setInsecure):
the server certificate is not verified. Configure a trusted CA before considering
the lab HTTPS identity-verification requirement complete.

Hardware check: upload a PNG on Flask, send show_image, observe it on the Atom,
and verify done on the page. Also test invalid image IDs, disconnect during download,
and reconnect after display to confirm result retries.
