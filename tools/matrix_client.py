#!/usr/bin/env python3
"""Command-line TCP client for the ESP8266 LED matrix controller."""

from __future__ import annotations

import argparse
import socket
import sys
from collections.abc import Sequence


MAGIC = bytes((0x4C, 0x4D))
VERSION = 0x01
RESPONSE_COMMAND = 0x80
MAX_PAYLOAD_SIZE = 255
RESPONSE_SIZE = 6

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


def rgb_payload(values: Sequence[int]) -> bytes:
    return bytes(parse_byte(value, "color") for value in values)


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
    fill.add_argument("red", type=lambda value: parse_byte(int(value), "red"))
    fill.add_argument("green", type=lambda value: parse_byte(int(value), "green"))
    fill.add_argument("blue", type=lambda value: parse_byte(int(value), "blue"))

    pixel = subparsers.add_parser("pixel")
    pixel.add_argument("x", type=lambda value: parse_byte(int(value), "x"))
    pixel.add_argument("y", type=lambda value: parse_byte(int(value), "y"))
    pixel.add_argument("red", type=lambda value: parse_byte(int(value), "red"))
    pixel.add_argument("green", type=lambda value: parse_byte(int(value), "green"))
    pixel.add_argument("blue", type=lambda value: parse_byte(int(value), "blue"))

    panel = subparsers.add_parser("panel")
    panel.add_argument("state", choices=("on", "off"))

    static = subparsers.add_parser("static")
    static.add_argument("red", type=lambda value: parse_byte(int(value), "red"))
    static.add_argument("green", type=lambda value: parse_byte(int(value), "green"))
    static.add_argument("blue", type=lambda value: parse_byte(int(value), "blue"))

    preset = subparsers.add_parser("preset")
    preset.add_argument("effect_id", type=lambda value: parse_byte(int(value), "effect_id"))
    preset.add_argument("--interval", type=int, default=140)
    preset.add_argument("--r", type=lambda value: parse_byte(int(value), "red"), default=255)
    preset.add_argument("--g", type=lambda value: parse_byte(int(value), "green"), default=255)
    preset.add_argument("--b", type=lambda value: parse_byte(int(value), "blue"), default=255)

    frame = subparsers.add_parser("frame")
    frame.add_argument("path", help="binary file containing 192 RGB bytes")

    return parser


def command_payload(args: argparse.Namespace) -> tuple[int, bytes]:
    if args.command in ("ping", "clear", "stop"):
        return COMMANDS[args.command], b""
    if args.command == "brightness":
        return COMMANDS["brightness"], bytes((args.value,))
    if args.command == "fill":
        return COMMANDS["fill"], rgb_payload((args.red, args.green, args.blue))
    if args.command == "pixel":
        return COMMANDS["set_pixel"], bytes((args.x, args.y, args.red, args.green, args.blue))
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

    raise ValueError(f"unsupported command: {args.command}")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
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
