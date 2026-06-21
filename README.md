# ESP8266 LED Matrix Controller

Wi-Fi TCP firmware for an ESP8266 NodeMCU driving a WS2812B 8x8 LED matrix.

The controller boots safely, joins Wi-Fi or opens its own access point, prints
its IP address, and accepts compact binary TCP packets for real-time LED control.

```text
client app  ->  TCP packet  ->  ESP8266  ->  WS2812B 8x8 matrix
```

## Features

- ESP8266 NodeMCU v2 target with Arduino + PlatformIO.
- WS2812B / NeoPixel matrix output on `D2 / GPIO4`.
- Resilient TCP server on port `7777`.
- Station mode with local `include/creds.h`.
- Access point fallback when Wi-Fi credentials are missing.
- Startup delay to reduce brownout and boot instability.
- Reconnect loop for Wi-Fi drops.
- TCP listener restart after reconnect or listener failure.
- Compact binary protocol for low-overhead matrix updates.
- Commands for ping, clear, brightness, fill, pixel, full frame, and panel on/off.
- Stored frame buffer: panel off blanks LEDs without forgetting the image.

## Hardware

Required:

- ESP8266 NodeMCU v2.
- WS2812B 8x8 matrix, 64 LEDs.
- External regulated 5V power supply.
- Common ground between ESP8266 and LED power supply.

Recommended:

- 330-470 ohm resistor in series with data near matrix `DIN`.
- 1000 uF capacitor across matrix `5V` and `GND`.
- Short data wire.
- Conservative brightness until the power supply is proven stable.

## Wiring

```text
ESP8266 D2 / GPIO4  ->  WS2812B DIN
ESP8266 GND         ->  PSU GND
PSU 5V              ->  WS2812B 5V
PSU GND             ->  WS2812B GND
```

The shared ground is mandatory. Without it, the data signal has no stable
reference and the matrix can flicker, show wrong colors, or partially work.

Do not power a full 64 LED matrix at high brightness from the ESP8266 5V pin.

## Project Layout

```text
include/
  AppConfig.h              hardware/network constants and local creds include
  LedMatrixController.h    matrix API and stored frame model
  MatrixProtocol.h         binary TCP protocol constants
  TcpMatrixServer.h        Wi-Fi, TCP server, parser, dispatch interface

src/
  main.cpp                 Arduino setup/loop composition
  LedMatrixController.cpp  NeoPixel rendering and matrix mapping
  MatrixProtocol.cpp       checksum helper
  TcpMatrixServer.cpp      Wi-Fi retry, TCP server, packet handling

platformio.ini             PlatformIO environment and cppcheck config
PLAN.md                    active improvement/refactoring roadmap
```

## Setup

Install PlatformIO, then install dependencies through the normal build:

```bash
pio run
```

### Wi-Fi Credentials

Create a local credentials file:

```bash
cp include/creds.example.h include/creds.h
```

Edit `include/creds.h`:

```cpp
#pragma once

#define WIFI_SSID "your-wifi"
#define WIFI_PASSWORD "your-password"
```

`include/creds.h` is ignored by git.

If `WIFI_SSID` is empty or `include/creds.h` is missing, the device starts an
open access point:

```text
SSID: led-matrix
IP:   192.168.4.1
TCP:  7777
```

## Build And Upload

Build:

```bash
pio run
```

Upload:

```bash
pio run --target upload
```

Open serial monitor:

```bash
pio device monitor
```

Expected boot output:

```text
ESP8266 WS2812B TCP matrix controller
LED data pin: D2 / GPIO4
Boot settle delay ms: 2000
Wi-Fi connected
Device IP: <ip-address>
TCP port: 7777
```

## TCP Protocol

Every command is one binary frame:

```text
byte 0      0x4C, 'L'
byte 1      0x4D, 'M'
byte 2      protocol version, currently 0x01
byte 3      command
byte 4      payload length, 0-192
bytes 5..N  payload
last byte   XOR checksum of every previous byte
```

The maximum payload is 192 bytes, enough for a full 64 pixel RGB frame.

### Commands

| Command | Name | Payload | Meaning |
|---:|---|---|---|
| `0x00` | ping | empty | Check protocol/socket health |
| `0x01` | clear | empty | Set stored frame to black |
| `0x02` | brightness | `brightness` | Set global brightness `0..255` |
| `0x03` | fill | `r g b` | Fill all pixels with one RGB color |
| `0x04` | set pixel | `x y r g b` | Set one logical matrix pixel |
| `0x05` | set frame | 192 RGB bytes | Replace full physical LED frame |
| `0x06` | panel enabled | `enabled` | `0` off, nonzero on |

Response frame:

```text
0x4C 0x4D 0x01 0x80 status checksum
```

Status values:

| Status | Meaning |
|---:|---|
| `0x00` | OK |
| `0x01` | Bad magic |
| `0x02` | Unsupported version |
| `0x03` | Unknown command |
| `0x04` | Invalid payload length or coordinate |
| `0x05` | Checksum mismatch |

## Packet Example

Fill the panel red:

```text
4C 4D 01 03 03 FF 00 00 E3
```

Explanation:

```text
4C 4D       magic "LM"
01          version
03          fill command
03          payload length
FF 00 00    RGB red
E3          XOR checksum
```

## Development

Format:

```bash
clang-format -i include/*.h src/*.cpp
```

Static check:

```bash
pio check
```

Full local verification:

```bash
clang-format -i include/*.h src/*.cpp
pio run
pio check
```

## Troubleshooting

Matrix flickers or only some LEDs work:

- Confirm ESP8266 `GND` and LED PSU `GND` are connected.
- Confirm data wire goes to matrix `DIN`, not `DOUT`.
- Lower brightness.
- Use a stronger 5V supply.
- Add the data resistor and power capacitor.

No serial output:

- Monitor speed is `115200`.
- ESP8266 ROM boot text may appear garbled before firmware starts.

Cannot connect over TCP:

- Check the serial monitor for `Device IP`.
- Confirm your computer is on the same Wi-Fi network or connected to the
  `led-matrix` access point.
- Use TCP port `7777`.

Wrong colors:

- The firmware assumes `NEO_GRB`.
- If red/green are swapped, update the NeoPixel color order in
  `LedMatrixController.cpp`.

## Roadmap

See [PLAN.md](PLAN.md) for the active refactoring and improvement roadmap.
