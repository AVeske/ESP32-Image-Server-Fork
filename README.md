# ESP32-Image-Server

Turn a small ESP32 display board into a **network-addressable screen**: push it a
picture over HTTP, keep a few live numbers on top of that picture with plain GET
requests, and let a tap or a button press flip its page — or flip another
device's page.

Two firmwares, one design:

| | [`atom-image-server`](atom-image-server/) | [`wt32-image-server`](wt32-image-server/) |
|---|---|---|
| Board | M5Stack **AtomS3R** | **WT32-SC01** (V3.2) |
| MCU | ESP32-S3, 8 MB flash | ESP32 WROVER + PSRAM |
| Panel | 0.85″ 128×128 IPS | 3.5″ 320×480 ST7796 |
| Input | one button (short / long / double) | 2×2 capacitive touch grid |
| Graphics | [M5Unified](https://github.com/m5stack/M5Unified) (M5GFX) | [LovyanGFX](https://github.com/lovyan03/LovyanGFX) |
| Frame size | 32 768 B RGB565 | 307 200 B RGB565 |
| Hardware status | **verified** | **builds clean, unverified on hardware** |

Both speak the same HTTP API, run the same UDP name discovery, and share
[`common/`](common/). Build with [PlatformIO](https://platformio.org/).

## Why

An ESP32 with a screen is usually programmed *for* a purpose. This makes it a
dumb, generic display endpoint instead: your program stays on the host, and the
device is one `curl` away. Some things people use it for:

- **status panels** — one image per state, flipped from a CI job or a script
- **machine HMIs** — a labelled diagram with live load/pressure/fill bars on it
- **props and signage** — a fleet of screens driving each other through taps
- **fiducial markers** — display an ArUco/AprilTag and change it on demand

## Quick start

```sh
git clone https://github.com/KKallas/ESP32-Image-Server
cd ESP32-Image-Server/atom-image-server      # or wt32-image-server
pio run -t upload
pio device monitor                            # 115200
```

Then, in the serial monitor:

```
wifi MyNetwork:my-password
ip
```

The device joins, saves the credentials, and prints its IP. (With no saved
network it starts a SoftAP — `AtomFramer` / `atomframer` — at `192.168.4.1`.)

Open `http://<ip>/` for the built-in framer UI: crop an image, **SEND → SLOT**,
and edit that slot's button/hotspot actions. Or drive it from a script:

```sh
curl "http://$IP/show?slot=2"                       # flip to slot 3
curl "http://$IP/set?n0=PRESSURE&b0=90"             # bar 0: "PRESSURE", 90%
curl "http://$IP/set?l0=RUNNING&b0=42&b1=17"        # label + two bars at once
curl "http://$IP/set?clear=1"                       # wipe the overlay
```

## The data overlay

Up to **4 text labels** and **4 percentage bars** are drawn on top of the
displayed image. They're set with query parameters on `GET /set` — one request
can carry as many as you like:

| parameter | meaning |
|-----------|---------|
| `l0`…`l3` | text of label *n* (labels stack down from the top-left); empty clears it |
| `b0`…`b3` | value of bar *n*, `0`–`100`; `-1` or empty hides that bar |
| `n0`…`n3` | caption shown above bar *n* |
| `on` | `0` stops drawing the overlay, `1` resumes (values are kept) |
| `clear` | drop every label and bar |

Bars stack up from the bottom edge with their caption and percentage above them;
sizes scale off the panel width, so the same call looks right on both boards. An
unrecognised key comes back as a `400` naming it, so a typo doesn't fail quietly.

The image itself is never modified — the overlay is painted over each redraw, so
a `/show` or a fresh `/frame` upload wipes it back to a clean picture. **Values
live in RAM only**: they are live telemetry, not configuration, and a reboot
comes back to the bare image.

`GET /overlay` reads the current values back, and they're included in `/state`.

## HTTP API

Identical on both boards, except where noted.

| Method | Path | Query / body | Effect |
|--------|------|--------------|--------|
| GET | `/` | — | the built-in framer UI |
| GET | `/state` | — | JSON: name, slot, filled[], overlay, battery |
| GET | `/show` | `?slot=N` | display stored slot N (0-based) |
| POST | `/frame` | `?slot=N` (`&mid=M` on Atom), multipart body | store RGB565 into slot N and show it |
| GET | `/frame` | `?slot=N` | stream slot N's raw RGB565 back |
| GET | `/set` | `?l0=…&b0=…` | update the data overlay |
| GET | `/overlay` | — | current overlay values |
| GET | `/buttons` | — | the button/hotspot action table |
| POST | `/buttons` | text body, one action per line | replace the action table |
| GET | `/peers` | — | discovered fleet devices |
| GET / POST | `/name` | — / text body | get / set this device's name |

`/state`:

```json
{
  "name": "red_oak", "slot": 0, "slots": 4,
  "filled": [true, false, false, false], "hasFrame": true,
  "battery": { "mv": 4050, "pct": 83 },
  "overlay": { "enabled": true, "labels": ["RUNNING","","",""],
               "bars": [{"name":"PRESSURE","pct":90}, {"name":"","pct":-1}] }
}
```

Uploads use the core `WebServer` multipart handler — binary-safe, no async-web
dependency. The framer page posts the packed bytes as a `FormData` blob.

## Slots and actions

Each device holds **4 slots** in LittleFS. The displayed slot survives a reboot,
and the panel comes straight back up on its image — no boot animation, no
waiting for WiFi.

Every slot maps its inputs to an action: three button gestures on the Atom
(short `<0.5 s` / long `>0.7 s` / double within `1.5 s`), four touch quadrants on
the WT32. An action is one line of text:

| value | effect |
|-------|--------|
| `1`–`4` | show that slot **on this device** |
| `red_oak:2` | show slot 2 on the fleet device **named** `red_oak` |
| `/show?slot=1` | this device, 0-based |
| `http://192.168.1.50/show?slot=0` | another unit, explicit IP |

Self-targeting actions are handled locally rather than over HTTP — the web
server is single-threaded, so a request to our own IP would deadlock. On the
WT32, quadrants sharing the same value flash together, so two cells pointing at
one endpoint read as a single larger button. An unconfigured **long press** on
the Atom falls back to showing the WiFi status on the LCD.

## Naming & discovery

Each device picks a persistent random `<colour>_<tree>` name (e.g. `red_oak`) on
first boot and broadcasts `"<name> <ip>"` over **UDP 50505** every ~8 s, while
listening to build a name→IP table of the fleet (16 peers, 90 s TTL). So an
action can address a peer by **name** instead of a brittle IP.

Because the table refreshes from live broadcasts and STA mode auto-reconnects,
**swapping the WiFi infrastructure** (same SSID, new router) just works: each
unit rejoins, re-announces its new address, and `red_oak:2` keeps resolving —
back to normal in well under a minute.

Rename via `POST /name`, the framer page, or the serial `name <new>` command.

## Serial console

115200 baud (`pio device monitor`):

| command | effect |
|---------|--------|
| `wifi <ssid>:<password>` | join that network; credentials are saved and re-joined on boot |
| `ip` / `status` | current mode / SSID / IP / RSSI |
| `name` / `name <new>` | show / set this device's name |
| `ap` | forget the saved network and start the SoftAP |
| `help` | list commands |

A bare `ssid:password` line works too. The split is on the **first** colon, so
the password may contain colons (the SSID may not). A failed join falls back to
the SoftAP after ~20 s.

[`atom-image-server/tools/atomctl.py`](atom-image-server/tools/atomctl.py) drives
this console from the host (`pip install pyserial`; close the serial monitor
first — only one program can own the port).

## Layout

```
common/
  names_discovery.h    UDP name broadcast + peer table
  overlay.h            labels/bars state, GET parsing, drawing
atom-image-server/     AtomS3R firmware  (M5Unified)
wt32-image-server/     WT32-SC01 firmware (LovyanGFX)
```

`overlay.h` is templated on the display type rather than typed against a base
class: M5GFX lives in namespace `m5gfx` and LovyanGFX in `lgfx`, so the two
boards share no base type even though the drawing API is identical.

Both firmwares are a single translation unit, so the shared headers use plain
file-scope statics.

## Next steps

- **Movement detection.** The AtomS3R has an onboard IMU; report motion in
  `/state` (moving / magnitude / shake events) and optionally let a shake or a
  pick-up fire an action, the way a button gesture does. The WT32-SC01 has no
  IMU and would report `null`.
- **Verify the WT32 on hardware.** It builds clean but the panel init, touch
  mapping and cross-device GETs have not been confirmed on a real unit.
- **Overlay in the framer UI.** Right now labels and bars are HTTP-only; the web
  page should preview and edit them.
- **More overlay marks.** Sparkline or gauge alongside the bar.
- **Port to other boards.** Anything LovyanGFX supports should need only a new
  panel config plus the frame dimensions.

## Licence

MIT — see [LICENSE](LICENSE).
