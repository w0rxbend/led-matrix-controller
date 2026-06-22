#!/usr/bin/env python3
"""Draw a single 8x8 heart frame."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from matrix_client import (  # noqa: E402
    COMMANDS,
    DEFAULT_LAYOUT,
    FRAME_BYTES,
    LAYOUTS,
    MatrixClient,
    make_heart_frame as make_protocol_heart_frame,
    parse_byte,
    report_status,
)


def make_heart_frame(
    red: int,
    green: int,
    blue: int,
    background: tuple[int, int, int],
    rotation: int = -90,
    layout: str = DEFAULT_LAYOUT,
) -> bytes:
    frame = bytearray(make_protocol_heart_frame(red, green, blue, rotation, layout))
    if background == (0, 0, 0):
        return bytes(frame)

    bg_red, bg_green, bg_blue = background
    for offset in range(0, FRAME_BYTES, 3):
        if frame[offset : offset + 3] == b"\x00\x00\x00":
            frame[offset : offset + 3] = bytes((bg_red, bg_green, bg_blue))
    return bytes(frame)


def main() -> int:
    parser = argparse.ArgumentParser(description="Draw a heart on the LED matrix")
    parser.add_argument("--host", default="192.168.1.127")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--r", type=lambda value: parse_byte(int(value), "red"), default=255)
    parser.add_argument("--g", type=lambda value: parse_byte(int(value), "green"), default=0)
    parser.add_argument("--b", type=lambda value: parse_byte(int(value), "blue"), default=80)
    parser.add_argument("--bg-r", type=lambda value: parse_byte(int(value), "background red"), default=0)
    parser.add_argument("--bg-g", type=lambda value: parse_byte(int(value), "background green"), default=0)
    parser.add_argument("--bg-b", type=lambda value: parse_byte(int(value), "background blue"), default=0)
    parser.add_argument("--rotation", type=int, choices=(-90, 0, 90, 180), default=-90)
    parser.add_argument("--layout", choices=LAYOUTS, default=DEFAULT_LAYOUT)
    args = parser.parse_args()

    frame = make_heart_frame(
        args.r, args.g, args.b, (args.bg_r, args.bg_g, args.bg_b), args.rotation, args.layout
    )
    with MatrixClient(args.host, args.port, args.timeout) as client:
        status = client.send_command(COMMANDS["set_frame"], frame)

    report_status(status)
    return 0 if status == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
