#!/usr/bin/env python3
"""letter_station — receive letters from the AtomS3R over the COM port and
forward them on as HTTP POSTs.

The firmware (LETTER_TRANSPORT = SERIAL_PORT) prints one line per letter:
    {"letter":"A"}
mixed in with its debug log. This picks those lines out and POSTs the same
JSON to --url.

Examples:
    python letter_station.py                                  # print only
    python letter_station.py --url http://localhost:5000/api/letter
    python letter_station.py --port COM5 --url http://10.0.0.7:5000/api/letter

Port is auto-detected (ESP32-S3 native USB); override with --port.
Needs pyserial:  pip install pyserial
Close `pio device monitor` first — only one program can own the port.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not found — install it with:  pip install pyserial")

ESP32S3_VID = 0x303A   # Espressif native-USB vendor id


def find_port():
    cands = []
    for p in list_ports.comports():
        if (p.vid == ESP32S3_VID
                or "usbmodem" in (p.device or "")
                or "usbserial" in (p.device or "")):
            cands.append(p.device)
    if len(cands) == 1:
        return cands[0]
    if not cands:
        sys.exit("No serial device found — plug in the AtomS3R or pass --port.")
    sys.exit("Multiple ports found, pick one with --port:\n  " + "\n  ".join(cands))


def parse_letter(line):
    """Return the letter if `line` is a {"letter":"X"} payload, else None."""
    if not line.startswith('{"letter"'):
        return None
    try:
        letter = json.loads(line).get("letter")
    except (ValueError, AttributeError):
        return None
    return letter if isinstance(letter, str) and len(letter) == 1 else None


def forward(url, letter, timeout):
    body = json.dumps({"letter": letter}).encode()
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            print(f"  -> {resp.status} {resp.read().decode('utf-8', 'replace').strip()}")
    except urllib.error.HTTPError as e:
        print(f"  -> {e.code} {e.read().decode('utf-8', 'replace').strip()}")
    except (urllib.error.URLError, OSError) as e:
        print(f"  -> failed: {e}")


def main():
    ap = argparse.ArgumentParser(description="Forward AtomS3R letters from the COM port.")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--url", help="POST each letter here as {\"letter\":\"X\"}; "
                                  "omit to only print")
    ap.add_argument("--timeout", type=float, default=3.0, help="HTTP timeout, seconds")
    ap.add_argument("--log", action="store_true", help="also print the device's debug lines")
    args = ap.parse_args()
    port = args.port or find_port()

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}\n(Is `pio device monitor` still running?)")

    time.sleep(0.3)          # let the CDC port settle
    ser.reset_input_buffer()
    print(f"[listening on {port}" + (f", forwarding to {args.url}]" if args.url else ", print only]"))

    buf = b""
    try:
        while True:
            data = ser.read(256)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                letter = parse_letter(line)
                if letter is None:
                    if args.log and line:
                        print(f"  | {line}")
                    continue
                print(f"letter {letter}")
                if args.url:
                    forward(args.url, letter, args.timeout)
    except KeyboardInterrupt:
        pass
    except serial.SerialException as e:
        sys.exit(f"Lost {port}: {e}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
