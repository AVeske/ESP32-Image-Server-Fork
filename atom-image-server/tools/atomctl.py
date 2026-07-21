#!/usr/bin/env python3
"""atomctl — drive the AtomS3R image server's serial console.

Set client (STA) or master (SoftAP) WiFi mode and read back the IP.

Examples:
    ./atomctl.py client MyWiFi "my password"   # join a network, wait for DHCP IP
    ./atomctl.py ip                              # ask for the current IP
    ./atomctl.py master                          # go back to SoftAP
    ./atomctl.py status
    ./atomctl.py raw "help"

Port is auto-detected (ESP32-S3 native USB); override with --port.
Needs pyserial:  pip install pyserial
Close `pio device monitor` first — only one program can own the port.
"""
import argparse
import sys
import time

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


def read_until(ser, seconds, stop=None):
    """Print incoming lines for up to `seconds`; return early if `stop(line)`."""
    deadline = time.time() + seconds
    buf = b""
    while time.time() < deadline:
        data = ser.read(256)
        if not data:
            time.sleep(0.05)
            continue
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            print(line)
            if stop and stop(line):
                return True
    return False


def send(ser, cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()


def main():
    ap = argparse.ArgumentParser(description="Drive the AtomS3R serial console.")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="seconds to wait for a DHCP IP in client mode")
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("client", help="join a WiFi network (STA), wait for the IP")
    c.add_argument("ssid")
    c.add_argument("password", nargs="?", default="")
    sub.add_parser("master", help="forget WiFi and start the SoftAP")
    sub.add_parser("ip", help="ask for the current IP")
    sub.add_parser("status", help="ask for mode / ssid / ip / rssi")
    r = sub.add_parser("raw", help="send a literal console line")
    r.add_argument("line")

    args = ap.parse_args()
    port = args.port or find_port()

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}\n(Is `pio device monitor` still running?)")

    time.sleep(0.3)          # let the CDC port settle
    ser.reset_input_buffer()

    if args.cmd == "client":
        # split is on the first colon device-side, so the password may contain ':'
        send(ser, f"wifi {args.ssid}:{args.password}")
        print(f"[joining {args.ssid!r}, waiting up to {args.timeout:.0f}s for DHCP...]")
        ok = read_until(ser, args.timeout,
                        stop=lambda l: l.startswith("IP ") or "timed out" in l)
        if not ok:
            print("[no IP within timeout — try `./atomctl.py ip` again]")
            sys.exit(2)
    elif args.cmd == "master":
        send(ser, "ap")
        read_until(ser, 3.0, stop=lambda l: l.startswith("IP "))
    elif args.cmd == "ip":
        send(ser, "ip")
        read_until(ser, 3.0, stop=lambda l: l.startswith("IP ") or "not connected" in l)
    elif args.cmd == "status":
        send(ser, "status")
        read_until(ser, 3.0, stop=lambda l: l.startswith("IP ") or "not connected" in l)
    elif args.cmd == "raw":
        send(ser, args.line)
        read_until(ser, 3.0)

    ser.close()


if __name__ == "__main__":
    main()
