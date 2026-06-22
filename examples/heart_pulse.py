#!/usr/bin/env python3
"""Upload a pulsing 8x8 heart animation."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))
sys.path.insert(0, str(PROJECT_ROOT / "examples"))

from heart_frame import FRAME_BYTES, make_heart_frame  # noqa: E402
from matrix_client import COMMANDS, MatrixClient, parse_byte, report_status  # noqa: E402


PULSE_LEVELS = (70, 110, 160, 220, 255, 220, 160, 110)


def scaled_color(red: int, green: int, blue: int, scale: int) -> tuple[int, int, int]:
    return (
        (red * scale) // 255,
        (green * scale) // 255,
        (blue * scale) // 255,
    )


def upload_frame(client: MatrixClient, index: int, count: int, delay_ms: int, frame: bytes) -> int:
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"frame must be {FRAME_BYTES} bytes")

    payload = bytes((index, count, delay_ms & 0xFF, (delay_ms >> 8) & 0xFF)) + frame
    return client.send_command(COMMANDS["upload_custom_frame"], payload)


def make_pulse_frames(red: int, green: int, blue: int, rotation: int = -90, layout: str = "h-tl") -> list[bytes]:
    frames: list[bytes] = []
    for scale in PULSE_LEVELS:
        frame_red, frame_green, frame_blue = scaled_color(red, green, blue, scale)
        frames.append(make_heart_frame(frame_red, frame_green, frame_blue, (0, 0, 0), rotation, layout))
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload a pulsing heart animation")
    parser.add_argument("--host", default="192.168.1.127")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--delay", type=int, default=90, help="per-frame delay in ms")
    parser.add_argument("--r", type=lambda value: parse_byte(int(value), "red"), default=255)
    parser.add_argument("--g", type=lambda value: parse_byte(int(value), "green"), default=0)
    parser.add_argument("--b", type=lambda value: parse_byte(int(value), "blue"), default=0)
    parser.add_argument("--rotation", type=int, choices=(-90, 0, 90, 180), default=-90)
    parser.add_argument("--layout", choices=("h-tl", "h-tr", "h-bl", "h-br"), default="h-tl")
    args = parser.parse_args()

    frames = make_pulse_frames(args.r, args.g, args.b, args.rotation, args.layout)
    with MatrixClient(args.host, args.port, args.timeout) as client:
        for index, frame in enumerate(frames):
            status = upload_frame(client, index, len(frames), args.delay, frame)
            report_status(status)
            if status != 0:
                return 1

    print(f"uploaded pulsing heart: frames={len(frames)} delay={args.delay}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
