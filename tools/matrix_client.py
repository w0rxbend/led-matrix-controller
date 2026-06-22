#!/usr/bin/env python3
"""Command-line TCP client for the ESP8266 LED matrix controller.

Most commands expose display-space coordinates: x grows left-to-right on every
visible row. The fixed firmware still uses its original serpentine mapping, so
this client translates display-space points before sending commands.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from collections.abc import Sequence


MAGIC = bytes((0x4C, 0x4D))
VERSION = 0x01
RESPONSE_COMMAND = 0x80
MAX_PAYLOAD_SIZE = 255
RESPONSE_SIZE = 6
MATRIX_WIDTH = 8
MATRIX_HEIGHT = 8
FRAME_BYTES = MATRIX_WIDTH * MATRIX_HEIGHT * 3
DEFAULT_LAYOUT = "h-tl"
LAYOUTS = ("h-tl", "h-tr", "h-bl", "h-br")

# The soldered controller cannot be reflashed, so client code owns all display
# compensation. Firmware still exposes a horizontal serpentine matrix:
#   row 1: left -> right
#   row 2: right -> left
#   row 3: left -> right
# but real drawings are correct only when client display-space coordinates
# pre-flip rows 2, 4, 6, and 8 before using set_pixel or packing full frames.

COMMANDS = {
    "ping": 0x00,
    "clear": 0x01,
    "brightness": 0x02,
    "fill": 0x03,
    "set_pixel": 0x04,
    "set_frame": 0x05,
    "panel_enabled": 0x06,
    "static": 0x07,
    "preset": 0x08,
    "upload_custom_frame": 0x09,
    "stop": 0x0A,
}

STATUS_NAMES = {
    0x00: "ok",
    0x01: "bad magic",
    0x02: "unsupported version",
    0x03: "unknown command",
    0x04: "invalid length",
    0x05: "checksum mismatch",
}

HEART_PATTERN = (
    "01100110",
    "11111111",
    "11111111",
    "11111111",
    "01111110",
    "00111100",
    "00011000",
    "00000000",
)

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


def checksum(data: bytes) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value


def build_frame(command: int, payload: bytes = b"") -> bytes:
    if not 0 <= command <= 0xFF:
        raise ValueError("command must be in range 0..255")
    if len(payload) > MAX_PAYLOAD_SIZE:
        raise ValueError(f"payload must be at most {MAX_PAYLOAD_SIZE} bytes")

    frame = MAGIC + bytes((VERSION, command, len(payload))) + payload
    return frame + bytes((checksum(frame),))


def read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("socket closed before response was complete")
        data.extend(chunk)
    return bytes(data)


def read_status(sock: socket.socket) -> int:
    response = read_exact(sock, RESPONSE_SIZE)
    if response[:2] != MAGIC:
        raise ValueError(f"bad response magic: {response.hex(' ')}")
    if response[2] != VERSION:
        raise ValueError(f"bad response version: 0x{response[2]:02x}")
    if response[3] != RESPONSE_COMMAND:
        raise ValueError(f"bad response command: 0x{response[3]:02x}")
    if checksum(response[:-1]) != response[-1]:
        raise ValueError(f"bad response checksum: {response.hex(' ')}")
    return response[4]


def parse_byte(value: int, name: str) -> int:
    if not 0 <= value <= 255:
        raise argparse.ArgumentTypeError(f"{name} must be in range 0..255")
    return value


def parse_coordinate(value: str, axis: str) -> int:
    coordinate = int(value)
    limit = MATRIX_WIDTH if axis == "x" else MATRIX_HEIGHT
    if not 0 <= coordinate < limit:
        raise argparse.ArgumentTypeError(f"{axis} must be in range 0..{limit - 1}")
    return coordinate


def rgb_payload(values: Sequence[int]) -> bytes:
    return bytes(parse_byte(value, "color") for value in values)


def physical_index(x: int, y: int, layout: str = DEFAULT_LAYOUT) -> int:
    """Return the firmware's raw physical byte index for server-space x/y."""
    if layout not in LAYOUTS:
        raise ValueError(f"layout must be one of {', '.join(LAYOUTS)}")

    if layout == "h-tl":
        if y % 2 == 0:
            return y * MATRIX_WIDTH + x
        return y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x)

    if layout == "h-tr":
        if y % 2 == 0:
            return y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x)
        return y * MATRIX_WIDTH + x

    physical_y = MATRIX_HEIGHT - 1 - y
    if layout == "h-bl":
        if y % 2 == 0:
            return physical_y * MATRIX_WIDTH + x
        return physical_y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x)

    if y % 2 == 0:
        return physical_y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x)
    return physical_y * MATRIX_WIDTH + x


def display_to_server_point(x: int, y: int) -> tuple[int, int]:
    """Translate visible display-space x/y into the fixed firmware's x/y."""
    if y % 2 == 1:
        return MATRIX_WIDTH - 1 - x, y
    return x, y


def display_physical_index(x: int, y: int, layout: str = DEFAULT_LAYOUT) -> int:
    """Return the physical byte index for a visible display-space x/y point."""
    server_x, server_y = display_to_server_point(x, y)
    return physical_index(server_x, server_y, layout)


def rotate_point(x: int, y: int, rotation: int) -> tuple[int, int]:
    if rotation == 0:
        return x, y
    if rotation == 90:
        return MATRIX_WIDTH - 1 - y, x
    if rotation == -90:
        return y, MATRIX_HEIGHT - 1 - x
    if rotation == 180:
        return MATRIX_WIDTH - 1 - x, MATRIX_HEIGHT - 1 - y

    raise ValueError("rotation must be one of -90, 0, 90, 180")


def set_frame_pixel(frame: bytearray, x: int, y: int, red: int, green: int, blue: int, layout: str) -> None:
    base = display_physical_index(x, y, layout) * 3
    frame[base] = red
    frame[base + 1] = green
    frame[base + 2] = blue


def make_heart_frame(red: int, green: int, blue: int, rotation: int, layout: str = DEFAULT_LAYOUT) -> bytes:
    frame = bytearray(FRAME_BYTES)

    for y, row in enumerate(HEART_PATTERN):
        for x, value in enumerate(row):
            if value == "1":
                rotated_x, rotated_y = rotate_point(x, y, rotation)
                set_frame_pixel(frame, rotated_x, rotated_y, red, green, blue, layout)

    return bytes(frame)


def heart_points(rotation: int) -> list[tuple[int, int]]:
    points: list[tuple[int, int]] = []
    for y, row in enumerate(HEART_PATTERN):
        for x, value in enumerate(row):
            if value == "1":
                points.append(rotate_point(x, y, rotation))
    return points


def scaled_color(red: int, green: int, blue: int, scale: int) -> tuple[int, int, int]:
    return (
        (red * scale) // 255,
        (green * scale) // 255,
        (blue * scale) // 255,
    )


def upload_custom_frame(
    client: "MatrixClient", index: int, count: int, delay_ms: int, frame: bytes
) -> int:
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"frame must be exactly {FRAME_BYTES} bytes")

    payload = bytes((index, count, delay_ms & 0xFF, (delay_ms >> 8) & 0xFF)) + frame
    return client.send_command(COMMANDS["upload_custom_frame"], payload)


def draw_heart_pixels(
    client: "MatrixClient",
    red: int,
    green: int,
    blue: int,
    rotation: int,
    brightness: int,
    clear_first: bool = True,
) -> int:
    status = client.send_command(COMMANDS["panel_enabled"], bytes((1,)))
    if status != 0:
        return status

    status = client.send_command(COMMANDS["brightness"], bytes((brightness,)))
    if status != 0:
        return status

    if clear_first:
        status = client.send_command(COMMANDS["clear"])
        if status != 0:
            return status

    for x, y in heart_points(rotation):
        server_x, server_y = display_to_server_point(x, y)
        payload = bytes((server_x, server_y, red, green, blue))
        status = client.send_command(COMMANDS["set_pixel"], payload)
        if status != 0:
            return status
    return 0


def report_status(status: int) -> None:
    name = STATUS_NAMES.get(status, "unknown status")
    print(f"status=0x{status:02x} {name}")


class MatrixClient:
    def __init__(self, host: str, port: int = 7777, timeout: float = 5.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None

    def __enter__(self) -> "MatrixClient":
        self.connect()
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> None:
        self.close()

    def connect(self) -> None:
        self.close()
        self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self._sock.settimeout(self.timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self) -> None:
        if self._sock is None:
            return
        self._sock.close()
        self._sock = None

    def send_command(self, command: int, payload: bytes = b"") -> int:
        if self._sock is None:
            raise ConnectionError("client is not connected")
        self._sock.sendall(build_frame(command, payload))
        return read_status(self._sock)


def add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", default="192.168.1.127", help="controller IP address")
    parser.add_argument("--port", type=int, default=7777, help="controller TCP port")
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")


def add_rgb_options(
    parser: argparse.ArgumentParser,
    red: int = 255,
    green: int = 255,
    blue: int = 255,
    option_style: str = "positional",
) -> None:
    if option_style == "short":
        parser.add_argument("--r", type=lambda value: parse_byte(int(value), "red"), default=red)
        parser.add_argument("--g", type=lambda value: parse_byte(int(value), "green"), default=green)
        parser.add_argument("--b", type=lambda value: parse_byte(int(value), "blue"), default=blue)
        return

    if option_style != "positional":
        raise ValueError("option_style must be 'positional' or 'short'")

    parser.add_argument("red", type=lambda value: parse_byte(int(value), "red"))
    parser.add_argument("green", type=lambda value: parse_byte(int(value), "green"))
    parser.add_argument("blue", type=lambda value: parse_byte(int(value), "blue"))


def add_heart_options(parser: argparse.ArgumentParser, blue: int) -> None:
    add_rgb_options(parser, red=255, green=0, blue=blue, option_style="short")
    parser.add_argument("--rotation", type=int, choices=(-90, 0, 90, 180), default=-90)
    parser.add_argument("--layout", choices=LAYOUTS, default=DEFAULT_LAYOUT)
    parser.add_argument("--transport", choices=("pixels", "frame"), default="pixels")
    parser.add_argument("--brightness", type=lambda value: parse_byte(int(value), "brightness"), default=40)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send TCP commands to the LED matrix controller")
    add_common_options(parser)
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("ping")
    subparsers.add_parser("clear")
    subparsers.add_parser("stop")

    brightness = subparsers.add_parser("brightness")
    brightness.add_argument("value", type=lambda value: parse_byte(int(value), "brightness"))

    fill = subparsers.add_parser("fill")
    add_rgb_options(fill)

    pixel = subparsers.add_parser("pixel")
    pixel.add_argument("x", type=lambda value: parse_coordinate(value, "x"))
    pixel.add_argument("y", type=lambda value: parse_coordinate(value, "y"))
    add_rgb_options(pixel)

    panel = subparsers.add_parser("panel")
    panel.add_argument("state", choices=("on", "off"))

    static = subparsers.add_parser("static")
    add_rgb_options(static)

    preset = subparsers.add_parser("preset")
    preset.add_argument("effect_id", type=lambda value: parse_byte(int(value), "effect_id"))
    preset.add_argument("--interval", type=int, default=140)
    add_rgb_options(preset, option_style="short")

    frame = subparsers.add_parser("frame")
    frame.add_argument("path", help="binary file containing 192 RGB bytes")

    heart = subparsers.add_parser("heart")
    add_heart_options(heart, blue=80)

    heart_beat = subparsers.add_parser("heart-beat")
    add_heart_options(heart_beat, blue=0)
    heart_beat.add_argument("--cycles", type=int, default=6)

    return parser


def command_payload(args: argparse.Namespace) -> tuple[int, bytes]:
    if args.command in ("ping", "clear", "stop"):
        return COMMANDS[args.command], b""
    if args.command == "brightness":
        return COMMANDS["brightness"], bytes((args.value,))
    if args.command == "fill":
        return COMMANDS["fill"], rgb_payload((args.red, args.green, args.blue))
    if args.command == "pixel":
        server_x, server_y = display_to_server_point(args.x, args.y)
        return COMMANDS["set_pixel"], bytes((server_x, server_y, args.red, args.green, args.blue))
    if args.command == "panel":
        return COMMANDS["panel_enabled"], bytes((1 if args.state == "on" else 0,))
    if args.command == "static":
        return COMMANDS["static"], rgb_payload((args.red, args.green, args.blue))
    if args.command == "preset":
        if not 0 <= args.interval <= 0xFFFF:
            raise ValueError("interval must be in range 0..65535")
        payload = bytes(
            (
                args.effect_id,
                args.interval & 0xFF,
                (args.interval >> 8) & 0xFF,
                args.r,
                args.g,
                args.b,
            )
        )
        return COMMANDS["preset"], payload
    if args.command == "frame":
        with open(args.path, "rb") as frame_file:
            payload = frame_file.read()
        if len(payload) != 64 * 3:
            raise ValueError("frame file must contain exactly 192 bytes")
        return COMMANDS["set_frame"], payload
    if args.command == "heart":
        if args.transport == "pixels":
            raise ValueError("heart pixels transport is handled separately")
        payload = make_heart_frame(args.r, args.g, args.b, args.rotation, args.layout)
        return COMMANDS["set_frame"], payload

    raise ValueError(f"unsupported command: {args.command}")


def send_heartbeat(args: argparse.Namespace) -> int:
    if args.transport == "pixels":
        cycles = max(1, args.cycles)
        with MatrixClient(args.host, args.port, args.timeout) as client:
            for _ in range(cycles):
                for scale, delay_ms in HEARTBEAT_STEPS:
                    red, green, blue = scaled_color(args.r, args.g, args.b, scale)
                    status = draw_heart_pixels(client, red, green, blue, args.rotation, args.brightness)
                    report_status(status)
                    if status != 0:
                        return 1
                    time.sleep(delay_ms / 1000)

        print(f"played beating heart: cycles={cycles}")
        return 0

    with MatrixClient(args.host, args.port, args.timeout) as client:
        for index, (scale, delay_ms) in enumerate(HEARTBEAT_STEPS):
            red, green, blue = scaled_color(args.r, args.g, args.b, scale)
            frame = make_heart_frame(red, green, blue, args.rotation, args.layout)
            status = upload_custom_frame(client, index, len(HEARTBEAT_STEPS), delay_ms, frame)
            report_status(status)
            if status != 0:
                return 1

    print(f"uploaded beating heart: frames={len(HEARTBEAT_STEPS)}")
    return 0


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        if args.command == "heart-beat":
            return send_heartbeat(args)

        if args.command == "heart" and args.transport == "pixels":
            with MatrixClient(args.host, args.port, args.timeout) as client:
                status = draw_heart_pixels(
                    client, args.r, args.g, args.b, args.rotation, args.brightness
                )
            report_status(status)
            return 0 if status == 0 else 1

        command, payload = command_payload(args)
        with MatrixClient(args.host, args.port, args.timeout) as client:
            status = client.send_command(command, payload)
    except (ConnectionError, OSError, ValueError) as exc:
        print(f"matrix_client: {exc}", file=sys.stderr)
        return 1

    report_status(status)
    return 0 if status == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
