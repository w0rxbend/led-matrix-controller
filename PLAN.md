# ESP8266 LED Matrix Controller Improvement Plan

## Goal

Make the ESP8266 + WS2812B matrix controller reliable, easy to understand, and
easy to extend from Arduino/PlatformIO. The device should:

- Wait after power-on to reduce brownout risk.
- Join configured Wi-Fi or start its own fallback access point.
- Print its IP address and TCP port on startup.
- Run a resilient TCP server.
- Accept compact TCP command packets for matrix color control.
- Support brightness, clear, full-frame updates, individual pixels, and panel
  on/off.
- Recover from Wi-Fi disconnects, TCP listener failures, and malformed packets.

## Current State

The project already has a solid baseline:

- PlatformIO target: `nodemcuv2` with Arduino framework.
- LED library: `adafruit/Adafruit NeoPixel`.
- Private Wi-Fi credentials are loaded from ignored `include/creds.h`.
- `include/creds.example.h` documents the credential format.
- Boot delay exists via `AppConfig::kBootSettleDelayMs`.
- Wi-Fi station/AP mode is implemented.
- TCP server runs on port `7777`.
- TCP server restarts after Wi-Fi reconnect or listener failure.
- Protocol supports:
  - ping
  - clear
  - brightness
  - fill
  - set pixel
  - set full frame
  - panel enabled/disabled
- Code is split into focused components:
  - `AppConfig`
  - `MatrixProtocol`
  - `LedMatrixController`
  - `TcpMatrixServer`
- Formatting/static analysis setup exists:
  - `.clang-format`
  - `.clang-tidy`
  - `pio check` with `cppcheck`

## Main Gaps To Address

1. Protocol parser and command dispatch are still coupled inside
   `TcpMatrixServer`.
2. There are no host-side tests for checksum, frame parsing, or command
   validation.
3. No client utility exists to send valid TCP frames from a laptop.
4. The protocol is documented, but not versioned strongly enough for future
   changes.
5. Runtime diagnostics are serial-only and could be more structured.
6. Panel state is in RAM only; after reset the matrix always starts blank.
7. There is no automated formatting command in PlatformIO or a script.

## Refactoring Plan

### Phase 1: Separate Protocol Parsing

Create a dedicated parser class:

- `include/MatrixFrameParser.h`
- `src/MatrixFrameParser.cpp`

Responsibilities:

- Accept bytes one at a time.
- Validate magic bytes, version, payload length, and checksum.
- Emit either:
  - complete validated frame
  - parser error status
  - "need more bytes"

Benefits:

- `TcpMatrixServer` becomes transport-only.
- Parser can be unit-tested without ESP8266 Wi-Fi.
- Malformed TCP stream handling becomes easier to reason about.

Suggested API:

```cpp
enum class ParseResult {
  kNeedMoreBytes,
  kFrameReady,
  kError,
};

class MatrixFrameParser {
 public:
  ParseResult parseByte(uint8_t value);
  void reset();
  uint8_t command() const;
  const uint8_t* payload() const;
  uint8_t payloadLength() const;
  MatrixProtocol::Status errorStatus() const;
};
```

### Phase 2: Separate Command Dispatch

Create a small dispatcher:

- `include/MatrixCommandHandler.h`
- `src/MatrixCommandHandler.cpp`

Responsibilities:

- Validate command-specific payload lengths.
- Call `LedMatrixController`.
- Return `MatrixProtocol::Status`.

Benefits:

- Transport code does not know LED details.
- Command behavior can be tested independently.
- Adding commands later is safer.

### Phase 3: Tighten LED Matrix State Model

Keep the current stored-frame approach, then clarify names:

- Rename `frameRgb_` to `desiredFrameRgb_`.
- Rename `enabled_` to `panelEnabled_`.
- Add accessors for tests/debug:
  - `bool isEnabled() const`
  - `uint8_t brightness() const`

Consider later:

- Persist last brightness in EEPROM/LittleFS.
- Persist last panel enabled state.
- Add a startup color or status blink only when explicitly enabled.

### Phase 4: Improve Network Resilience

Current retry behavior is good. Improve observability and edge cases:

- Print Wi-Fi SSID in station mode without printing password.
- Print reconnect success after a retry, not only initial connection.
- Track last network state to avoid repeated log spam.
- Consider fallback AP after repeated station failures if desired.

Recommended behavior:

- If `WIFI_SSID` is empty: AP mode only.
- If `WIFI_SSID` exists: station retry forever.
- Optional future mode: station first, AP fallback after N failed attempts.

### Phase 5: Protocol Evolution

Keep the current compact frame:

```text
'L' 'M' version command payloadLength payload checksum
```

Near-term improvements:

- Add status command `0x07`:
  - returns brightness, panel enabled flag, matrix size, protocol version.
- Add protocol constants for expected payload sizes.
- Add named payload indexes for readability.
- Keep command IDs stable once a client exists.

Possible future commands:

- `0x07`: get status
- `0x08`: set logical full frame, ordered by x/y rows instead of physical LED
  chain order
- `0x09`: set boot behavior
- `0x0A`: save current brightness/panel state

## PlatformIO And Arduino Tooling Plan

### Build

Use:

```bash
pio run
```

### Upload

Use:

```bash
pio run --target upload
```

### Serial Monitor

Use:

```bash
pio device monitor
```

Expected startup output should include:

```text
ESP8266 WS2812B TCP matrix controller
LED data pin: D2 / GPIO4
Boot settle delay ms: 2000
Wi-Fi connected
Device IP: <ip>
TCP port: 7777
```

### Static Check

Use:

```bash
pio check
```

Current `cppcheck` setup is acceptable. Keep dependency checks skipped because
third-party library style warnings are not useful for firmware work.

### Formatting

Use:

```bash
clang-format -i include/*.h src/*.cpp
```

Recommended improvement: add a helper script or PlatformIO extra target:

- `scripts/format.sh`
- optional `extra_scripts` target later

### Unit Tests

PlatformIO unit tests should focus on host-compatible logic first:

- `MatrixProtocol::checksum`
- future `MatrixFrameParser`
- command payload validation

Suggested test layout:

```text
test/
  test_protocol/
    test_main.cpp
  test_parser/
    test_main.cpp
```

ESP8266 hardware tests can come later because they require a connected board.

## Client Tool Plan

Add a small local TCP client utility for manual testing:

```text
tools/
  matrix_client.py
```

Commands:

```bash
python tools/matrix_client.py --host 192.168.1.50 ping
python tools/matrix_client.py --host 192.168.1.50 fill 255 0 0
python tools/matrix_client.py --host 192.168.1.50 brightness 20
python tools/matrix_client.py --host 192.168.1.50 panel off
python tools/matrix_client.py --host 192.168.1.50 panel on
```

Benefits:

- Confirms protocol framing from a real client.
- Gives repeatable manual tests after upload.
- Avoids manually crafting binary frames.

Use [CLIENT_PROTOCOL.md](CLIENT_PROTOCOL.md) as the source of truth for client
behavior. Any future client utility should follow that document for frame
building, response validation, reconnect behavior, and animation pacing.

## Documentation Improvements

Keep `WS2812B_RF_NANO_CONTROLLER_PLAN.md` only if it remains useful, but it is
now misnamed for the ESP8266 direction. Recommended actions:

1. Rename it to `ESP8266_WS2812B_TCP_CONTROLLER_PLAN.md`.
2. Keep `PLAN.md` as the active engineering roadmap.
3. Add a concise `README.md` with:
   - wiring diagram text
   - build/upload commands
   - credential setup
   - TCP protocol summary
   - example client commands

## Hardware Reliability Checklist

Required wiring:

- ESP8266 `D2 / GPIO4` -> WS2812B `DIN`.
- LED external PSU `5V` -> matrix `5V`.
- LED external PSU `GND` -> matrix `GND`.
- ESP8266 `GND` -> LED PSU `GND`.

Recommended:

- 330-470 ohm resistor in series with data line near matrix DIN.
- 1000 uF capacitor across matrix 5V/GND.
- Keep data wire short.
- Use external 5V supply sized for brightness target.
- Keep default brightness conservative.

## Reasonable Enhancements

These are useful next improvements that keep the project practical for an
ESP8266 and avoid turning the firmware into a large application.

### Status And Diagnostics

- Add `get status` command `0x07` returning:
  - protocol version
  - matrix width/height
  - current brightness
  - panel enabled flag
  - Wi-Fi mode: station or AP
  - server port
- Add concise reconnect logs:
  - station reconnect started
  - station reconnect succeeded with IP
  - TCP server restarted
- Add an optional boot self-test command instead of running LED animations on
  every boot.

### Safer Power Behavior

- Add configurable maximum brightness in `AppConfig`.
- Clamp incoming brightness to that maximum unless a compile-time override is
  enabled.
- Add a "night mode" preset command that uses a very low brightness and warm
  color.
- Consider a soft-start helper that fades from black to the requested frame
  after boot or panel-on.

### Usability

- Add `tools/matrix_client.py` for repeatable manual testing from a laptop.
- Add sample commands in `README.md` after the client tool exists.
- Add a simple "logical frame" command so clients can send pixels in row-major
  x/y order without knowing the physical serpentine wiring.
- Add named constants for every payload size and payload byte offset.

### Reliability

- Add a parser timeout: if a partial TCP frame is not completed within a short
  window, reset the parser state.
- Disconnect clients that repeatedly send malformed frames.
- Track the active client remote IP in serial logs.
- Consider a watchdog-friendly `yield()` in long frame render paths if future
  matrix sizes grow beyond 8x8.

### Persistence

- Persist only low-risk settings:
  - brightness
  - panel enabled flag
  - optional max brightness
- Avoid writing on every command. Save only on explicit `save settings` command
  to protect flash lifetime.

### Testing

- Add host tests for checksum and future parser behavior.
- Add test vectors for every command frame.
- Add one hardware smoke-test checklist for upload + serial monitor + TCP fill.

## Implementation Order

1. Rename the old plan file to match ESP8266 scope.
2. Add `README.md` for setup and protocol basics.
3. Add `tools/matrix_client.py`.
4. Extract `MatrixFrameParser`.
5. Extract `MatrixCommandHandler`.
6. Add PlatformIO unit tests for protocol/parser logic.
7. Add status command `0x07`.
8. Add parser timeout for incomplete TCP frames.
9. Add logical row-major frame command.
10. Add optional persistence for brightness/panel enabled state.
11. Add configurable maximum brightness clamp.
12. Add formatting helper script.
13. Run full verification:

```bash
clang-format -i include/*.h src/*.cpp
pio run
pio check
pio test
```

## Acceptance Criteria

The firmware is ready for regular use when:

- It boots with the configured delay.
- It prints IP and TCP port reliably.
- It reconnects Wi-Fi after router restart.
- It restarts the TCP listener after reconnect.
- It rejects malformed packets without crashing.
- It accepts fill, pixel, frame, brightness, clear, and panel on/off commands.
- It preserves the current frame while the panel is off.
- `pio run` passes.
- `pio check` passes with no project defects.
- Host-side protocol/parser tests pass.
- A client utility can control the matrix without custom byte crafting.
