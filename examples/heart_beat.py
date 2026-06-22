#!/usr/bin/env python3
"""Upload a red heartbeat-style 8x8 heart animation."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))
sys.path.insert(0, str(PROJECT_ROOT / "examples"))

from heart_frame import FRAME_BYTES, make_heart_frame  # noqa: E402
from heart_pulse import scaled_color, upload_frame  # noqa: E402
from matrix_client import MatrixClient, parse_byte, report_status  # noqa: E402


HEARTBEAT_STEPS = (
    (55, 140),
    (255, 70),
    (120, 90),
    (45, 90),
    (210, 80),
    (90, 120),
    (35, 260),
    (35, 180),
)


def make_heartbeat_frames(
    red: int, green: int, blue: int, rotation: int = -90, layout: str = "h-tl"
) -> list[tuple[int, bytes]]:
    frames: list[tuple[int, bytes]] = []
    for scale, delay_ms in HEARTBEAT_STEPS:
        frame_red, frame_green, frame_blue = scaled_color(red, green, blue, scale)
        frames.append(
            (delay_ms, make_heart_frame(frame_red, frame_green, frame_blue, (0, 0, 0), rotation, layout))
        )
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload a red beating heart animation")
    parser.add_argument("--host", default="192.168.1.127")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--r", type=lambda value: parse_byte(int(value), "red"), default=255)
    parser.add_argument("--g", type=lambda value: parse_byte(int(value), "green"), default=0)
    parser.add_argument("--b", type=lambda value: parse_byte(int(value), "blue"), default=0)
    parser.add_argument("--rotation", type=int, choices=(-90, 0, 90, 180), default=-90)
    parser.add_argument("--layout", choices=("h-tl", "h-tr", "h-bl", "h-br"), default="h-tl")
    args = parser.parse_args()

    frames = make_heartbeat_frames(args.r, args.g, args.b, args.rotation, args.layout)
    with MatrixClient(args.host, args.port, args.timeout) as client:
        for index, (delay_ms, frame) in enumerate(frames):
            if len(frame) != FRAME_BYTES:
                raise ValueError(f"frame must be {FRAME_BYTES} bytes")

            status = upload_frame(client, index, len(frames), delay_ms, frame)
            report_status(status)
            if status != 0:
                return 1

    print(f"uploaded beating heart: frames={len(frames)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
