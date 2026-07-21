#!/usr/bin/env python3
"""Generate an ArUco marker and push it to the AtomS3R image server.

Renders a DICT_4X4_50 marker (the dictionary the calibration prototype uses) as a
128x128 image with a white quiet zone, packs it RGB565 big-endian (32768 bytes),
and POSTs it to POST /frame as a multipart upload.

    ./send_aruco.py 7 172.16.5.27
    ./send_aruco.py 7 http://172.16.5.27/ --dict 4X4_50 --border 16

Needs: opencv-python, numpy, requests.
"""
import argparse
import sys

import cv2
import numpy as np
import requests

SIZE = 128  # device panel is 128x128


def make_frame(marker_id, dict_name, border):
    aruco_dict = cv2.aruco.getPredefinedDictionary(getattr(cv2.aruco, "DICT_" + dict_name))
    side = SIZE - 2 * border
    marker = cv2.aruco.generateImageMarker(aruco_dict, marker_id, side)  # black/white
    canvas = np.full((SIZE, SIZE), 255, np.uint8)                        # white quiet zone
    canvas[border:border + side, border:border + side] = marker
    # pack RGB565, big-endian (high byte first) — matches the ATOM FRAMER page
    out = bytearray(SIZE * SIZE * 2)
    j = 0
    for v in canvas.flatten():
        v = int(v)
        rgb565 = ((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3)
        out[j] = (rgb565 >> 8) & 0xFF
        out[j + 1] = rgb565 & 0xFF
        j += 2
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Send an ArUco marker to the AtomS3R.")
    ap.add_argument("id", type=int, help="marker id")
    ap.add_argument("host", help="device host or URL, e.g. 172.16.5.27")
    ap.add_argument("--dict", default="4X4_50", help="ArUco dictionary (default 4X4_50)")
    ap.add_argument("--border", type=int, default=16, help="white quiet-zone width in px")
    args = ap.parse_args()

    url = args.host if args.host.startswith("http") else "http://" + args.host
    url = url.rstrip("/") + "/frame"

    try:
        data = make_frame(args.id, args.dict, args.border)
    except Exception as e:
        sys.exit(f"could not render marker: {e}")

    print(f"sending DICT_{args.dict} #{args.id} ({len(data)} bytes) -> {url}")
    try:
        r = requests.post(url, files={"frame": ("frame.dat", data, "application/octet-stream")},
                          timeout=10)
    except requests.RequestException as e:
        sys.exit(f"POST failed: {e}")
    print(f"HTTP {r.status_code}: {r.text.strip()}")
    sys.exit(0 if r.ok else 1)


if __name__ == "__main__":
    main()
