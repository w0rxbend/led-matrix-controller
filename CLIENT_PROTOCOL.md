# Client Protocol Guide

This document explains how to implement a TCP client for the ESP8266 LED matrix
controller.

The server listens on TCP port `7777` and accepts compact binary frames. It does
not use HTTP, WebSocket, JSON, MQTT, or newline-delimited text.

## Connection Model

1. Power on the ESP8266 controller.
2. Read the serial monitor and find:

   ```text
   Device IP: <ip-address>
   TCP port: 7777
   ```

3. Open a TCP socket to `<ip-address>:7777`.
4. Send one complete binary command frame.
5. Read one 6-byte response frame.
6. Repeat commands on the same socket, or reconnect later.

The firmware is designed for one active client at a time. A client should keep
the socket open while it is actively controlling the matrix and reconnect if the
socket closes.

## Transport Rules

- TCP is a byte stream, not a packet stream.
- A single command may arrive at the server split across multiple TCP reads.
- A response may arrive at the client split across multiple TCP reads.
- Client code must read until it has exactly the expected number of response
  bytes.
- Do not rely on newline characters or text delimiters. There are none.
- Disable client-side buffering delays where possible for real-time animation.

Recommended client behavior:

- Use a connect timeout around `2-5s`.
- Use a read timeout around `1-2s` for command responses.
- Reconnect on socket errors.
- Send a `ping` command after reconnect to confirm protocol health.
- For animations, reuse the same TCP connection instead of connecting per frame.

## Byte Order And Values

All values are unsigned single bytes unless stated otherwise.

The current protocol does not use multi-byte integers, so there is no endianness
concern.

Color values are RGB in client payloads:

```text
r: 0..255
g: 0..255
b: 0..255
```

The firmware internally uses `NEO_GRB` for the WS2812B wire order, but clients
should always send normal RGB.

## Command Frame Format

Every client-to-device command has this shape:

```text
byte 0      magic 0x4C ('L')
byte 1      magic 0x4D ('M')
byte 2      version 0x01
byte 3      command
byte 4      payload length, 0..255
bytes 5..N  payload
last byte   checksum
```

The checksum is XOR of every previous byte in the frame:

```text
checksum = byte0 ^ byte1 ^ byte2 ^ ... ^ lastPayloadByte
```

The maximum payload length is `255`. A full frame is still:

```text
64 pixels * 3 RGB bytes = 192 bytes
```

## Response Frame Format

After every complete command frame, the device responds with exactly 6 bytes:

```text
byte 0  magic 0x4C ('L')
byte 1  magic 0x4D ('M')
byte 2  version 0x01
byte 3  response command 0x80
byte 4  status
byte 5  checksum
```

The response checksum is the same XOR algorithm over bytes `0..4`.

Client code should validate:

- magic bytes are `0x4C 0x4D`
- version is `0x01`
- response command is `0x80`
- checksum is correct
- status is `0x00` before treating the command as successful

## Status Codes

| Status | Name | Meaning | Client action |
|---:|---|---|---|
| `0x00` | OK | Command was accepted and applied | Continue |
| `0x01` | Bad magic | Frame did not start with `LM` | Rebuild frame or reconnect |
| `0x02` | Unsupported version | Version byte is not supported | Update client/server |
| `0x03` | Unknown command | Command byte is not implemented | Fix command id |
| `0x04` | Invalid payload length | Payload length or coordinate is invalid | Fix payload |
| `0x05` | Checksum mismatch | Frame was corrupted or checksum is wrong | Resend frame |

## Commands

### `0x00`: Ping

Checks whether the socket and protocol are alive.

Payload length: `0`

Payload: empty

Example frame:

```text
4C 4D 01 00 00 00
```

Expected status: `0x00`

### `0x01`: Clear

Sets the stored frame to black and renders it.

Payload length: `0`

Payload: empty

Use this when you want to forget the current image. If you only want to hide the
panel temporarily, use `set panel enabled` with `0`.

### `0x02`: Set Brightness

Sets global brightness.

Payload length: `1`

Payload:

```text
brightness
```

Brightness range: `0..255`

Practical recommendation:

- Start with `10..40`.
- Use high values only with a strong external 5V supply.

Example:

```text
brightness 20 -> payload: 14
```

### `0x03`: Fill Matrix

Sets every pixel to one RGB color.

Payload length: `3`

Payload:

```text
r g b
```

Examples:

```text
red    -> FF 00 00
green  -> 00 FF 00
blue   -> 00 00 FF
white  -> FF FF FF
black  -> 00 00 00
```

### `0x04`: Set Pixel

Sets one logical x/y pixel.

Payload length: `5`

Payload:

```text
x y r g b
```

Coordinate range:

```text
x: 0..7
y: 0..7
```

Coordinates are logical matrix coordinates. The firmware maps them to the
physical serpentine LED order.

Client note: `tools/matrix_client.py` exposes display-space coordinates for
drawn content. On the soldered panel, rows 2, 4, 6, and 8 are visibly mirrored
unless the client pre-flips those rows before calling this command.

Logical layout:

```text
(0,0) top-left      (7,0) top-right
(0,7) bottom-left   (7,7) bottom-right
```

### `0x05`: Set Full Frame

Replaces the complete stored frame.

Payload length: `192`

Payload:

```text
pixel0_r pixel0_g pixel0_b pixel1_r pixel1_g pixel1_b ... pixel63_r pixel63_g pixel63_b
```

Important: this command uses physical LED chain order, not logical x/y row
order. For the current horizontal zigzag matrix, row `0` is left-to-right, row
`1` is right-to-left, and rows continue alternating.

Client note: generated full-frame helpers in `tools/matrix_client.py` pack from
display-space coordinates and apply the same alternating-row compensation before
sending the 192-byte payload.

This is the most efficient command for animations because one TCP command
updates the whole matrix.

### `0x06`: Set Panel Enabled

Turns visible output off or on.

Payload length: `1`

Payload:

```text
enabled
```

Values:

```text
0      panel off
1..255 panel on
```

Panel off writes black to the LEDs but keeps the stored frame in RAM. Panel on
restores that stored frame.

Use this for temporary blanking. Use `clear` when you want to erase the frame.

### `0x07`: Set Static Color

Starts a fixed-color static mode.

Payload length: `3`

Payload:

```text
r g b
```

The matrix keeps this color until another mode change or direct command occurs.

### `0x08`: Set Preset Effect

Starts one preset effect.

Payload length: `6`

Payload:

```text
effect_id interval_lsb interval_msb r g b
```

`effect_id` values:

- `1` chase
- `2` color_wipe
- `3` blink
- `4` wave
- `5` rain
- `6` meteor
- `7` rainbow
- `8` breathing
- `9` scanner
- `10` sparkle
- `11` fire
- `12` matrix_rain
- `13` ripple
- `14` theater_chase
- `15` twinkle
- `16` comet
- `17` plasma
- `18` diagonal
- `19` border_chase
- `20` heartbeat
- `21` pulse_wipe
- `22` confetti
- `0` stop effect and return to direct mode

The interval controls frame timing in milliseconds. `0` is treated as `140ms`.

### `0x09`: Upload Custom Frame

Uploads one frame to the custom animation slot.

Payload length: `196`

Payload:

```text
frame_index frame_count delay_lsb delay_msb pixel0_r ... pixel63_b
```

Frame data is in physical LED order (same format as `0x05`).
Send all frames with the same `frame_count` (from index `0` to `frame_count-1`).
The device starts looping as soon as all frames are uploaded.

### `0x0A`: Stop Effect

Stops running preset/custom animation and returns to direct mode.

Payload length: `0`

## Building Frames

Pseudo-code:

```text
function buildFrame(command, payload):
    frame = [0x4C, 0x4D, 0x01, command, len(payload)] + payload
    checksum = 0
    for byte in frame:
        checksum = checksum XOR byte
    append checksum to frame
    return frame
```

## Reading Responses

Pseudo-code:

```text
function readExactly(socket, count):
    buffer = []
    while len(buffer) < count:
        chunk = socket.read(count - len(buffer))
        if chunk is empty:
            raise disconnected
        append chunk to buffer
    return buffer

function readResponse(socket):
    response = readExactly(socket, 6)
    validate magic, version, response command
    validate checksum
    return response[4]  // status
```

## Python Client Example

Minimal Python 3 client:

```python
import socket
import sys

MAGIC = [0x4C, 0x4D]
VERSION = 0x01
RESPONSE = 0x80

PING = 0x00
CLEAR = 0x01
BRIGHTNESS = 0x02
FILL = 0x03
SET_PIXEL = 0x04
SET_FRAME = 0x05
PANEL_ENABLED = 0x06


def checksum(data: bytes) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value


def build_frame(command: int, payload: bytes = b"") -> bytes:
    if len(payload) > 192:
        raise ValueError("payload too large")

    frame = bytes(MAGIC + [VERSION, command, len(payload)]) + payload
    return frame + bytes([checksum(frame)])


def read_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("socket closed")
        chunks.extend(chunk)
    return bytes(chunks)


def read_status(sock: socket.socket) -> int:
    response = read_exact(sock, 6)

    if response[0] != MAGIC[0] or response[1] != MAGIC[1]:
        raise ValueError(f"bad response magic: {response.hex()}")
    if response[2] != VERSION:
        raise ValueError(f"bad response version: {response[2]}")
    if response[3] != RESPONSE:
        raise ValueError(f"not a status response: {response[3]}")
    if checksum(response[:5]) != response[5]:
        raise ValueError(f"bad response checksum: {response.hex()}")

    return response[4]


def send_command(sock: socket.socket, command: int, payload: bytes = b"") -> None:
    sock.sendall(build_frame(command, payload))
    status = read_status(sock)
    if status != 0:
        raise RuntimeError(f"device returned status 0x{status:02x}")


def main() -> None:
    host = sys.argv[1]
    port = 7777

    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(2)

        send_command(sock, PING)
        send_command(sock, BRIGHTNESS, bytes([20]))
        send_command(sock, FILL, bytes([255, 0, 0]))
        send_command(sock, SET_PIXEL, bytes([0, 0, 255, 255, 255]))
        send_command(sock, PANEL_ENABLED, bytes([0]))
        send_command(sock, PANEL_ENABLED, bytes([1]))


if __name__ == "__main__":
    main()
```

Run:

```bash
python client.py 192.168.1.50
```

## JavaScript / Node.js Client Notes

Use `net.Socket`.

Key points:

- Use `socket.write(Buffer.from(frame))`.
- Buffer incoming data until at least 6 response bytes are available.
- Do not assume the `data` event contains exactly one response.
- Keep a command queue so each command waits for its response before sending the
  next command.

Frame builder:

```js
function checksum(buffer) {
  let value = 0;
  for (const byte of buffer) {
    value ^= byte;
  }
  return value;
}

function buildFrame(command, payload = []) {
  if (payload.length > 192) {
    throw new Error("payload too large");
  }

  const frame = Buffer.from([0x4c, 0x4d, 0x01, command, payload.length, ...payload]);
  return Buffer.concat([frame, Buffer.from([checksum(frame)])]);
}
```

## Animation Client Guidance

For animations:

- Open one TCP connection and keep it open.
- Set brightness once at the beginning.
- Send repeated `set frame` commands.
- Wait for each `OK` response before sending the next frame.
- Keep frame rate modest on ESP8266; start with `10-20 FPS`.
- Avoid full white at high brightness unless the power supply is sized for it.

Frame timing example:

```text
20 FPS = one frame every 50 ms
10 FPS = one frame every 100 ms
```

The WS2812B update itself takes time, and Wi-Fi latency is variable, so clients
should tolerate jitter.

## Reconnect Strategy

Recommended client reconnect loop:

1. Connect to `<device-ip>:7777`.
2. Send `ping`.
3. If ping succeeds, send commands normally.
4. If any socket read/write fails, close the socket.
5. Wait `500-2000 ms`.
6. Reconnect and ping again.

When the ESP8266 reconnects Wi-Fi, it may restart the TCP listener. Existing
client sockets should be treated as disposable.

## Common Client Bugs

- Sending ASCII hex text like `"4C4D01000000"` instead of binary bytes.
- Forgetting the checksum byte.
- Computing checksum over the payload only instead of the header plus payload.
- Reading fewer than 6 response bytes and treating that as a full response.
- Opening and closing a TCP socket for every animation frame.
- Sending logical x/y ordered full frames to command `0x05`, which expects
  physical LED order.
- Using brightness `255` during early power testing.

## Test Frames

Ping:

```text
4C 4D 01 00 00 00
```

Clear:

```text
4C 4D 01 01 00 01
```

Brightness 20:

```text
4C 4D 01 02 01 14 15
```

Fill red:

```text
4C 4D 01 03 03 FF 00 00 E3
```

Panel off:

```text
4C 4D 01 06 01 00 01
```

Panel on:

```text
4C 4D 01 06 01 01 00
```
