#!/usr/bin/env python3
"""Generate and upload one custom animation slot.

The firmware accepts one custom animation slot. Each uploaded frame payload is:

  frame_index, frame_count, delay_lsb, delay_msb, 192 RGB bytes

The 192 RGB bytes are physical LED order, so this example includes the same
serpentine mapping used by the firmware.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from matrix_client import COMMANDS, MatrixClient, report_status  # noqa: E402


WIDTH = 8
HEIGHT = 8
LED_COUNT = WIDTH * HEIGHT
FRAME_BYTES = LED_COUNT * 3


def physical_index(x: int, y: int) -> int:
    """Map logical x/y to the physical serpentine matrix order."""
    if y % 2 == 0:
        return y * WIDTH + x
    return y * WIDTH + (WIDTH - 1 - x)


def empty_frame() -> bytearray:
    return bytearray(FRAME_BYTES)


def set_pixel(frame: bytearray, x: int, y: int, red: int, green: int, blue: int) -> None:
    base = physical_index(x, y) * 3
    frame[base] = red
    frame[base + 1] = green
    frame[base + 2] = blue


def make_bouncing_dot_frames(red: int, green: int, blue: int) -> list[bytes]:
    positions = [
        (0, 0),
        (1, 1),
        (2, 2),
        (3, 3),
        (4, 4),
        (5, 5),
        (6, 6),
        (7, 7),
        (6, 6),
        (5, 5),
        (4, 4),
        (3, 3),
        (2, 2),
        (1, 1),
    ]

    frames: list[bytes] = []
    for x, y in positions:
        frame = empty_frame()
        set_pixel(frame, x, y, red, green, blue)
        frames.append(bytes(frame))
    return frames


def make_wipe_frames(red: int, green: int, blue: int) -> list[bytes]:
    frames: list[bytes] = []
    for count in range(1, LED_COUNT + 1):
        frame = empty_frame()
        for logical_index in range(count):
            x = logical_index % WIDTH
            y = logical_index // WIDTH
            set_pixel(frame, x, y, red, green, blue)
        frames.append(bytes(frame))
    return frames


def upload_frame(client: MatrixClient, index: int, count: int, delay_ms: int, frame: bytes) -> int:
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"frame must be {FRAME_BYTES} bytes")

    payload = bytes((index, count, delay_ms & 0xFF, (delay_ms >> 8) & 0xFF)) + frame
    return client.send_command(COMMANDS["upload_custom_frame"], payload)


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload generated custom animation frames")
    parser.add_argument("--host", default="192.168.1.127")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--delay", type=int, default=120, help="per-frame delay in ms")
    parser.add_argument("--mode", choices=["dot", "wipe"], default="dot")
    parser.add_argument("--r", type=int, default=0)
    parser.add_argument("--g", type=int, default=180)
    parser.add_argument("--b", type=int, default=255)
    args = parser.parse_args()

    frames = (
        make_bouncing_dot_frames(args.r, args.g, args.b)
        if args.mode == "dot"
        else make_wipe_frames(args.r, args.g, args.b)
    )

    if len(frames) > 8:
        frames = frames[:8]

    with MatrixClient(args.host, args.port) as client:
        for index, frame in enumerate(frames):
            status = upload_frame(client, index, len(frames), args.delay, frame)
            report_status(status)
            if status != 0:
                return 1

    print(f"uploaded custom animation: frames={len(frames)} delay={args.delay}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
