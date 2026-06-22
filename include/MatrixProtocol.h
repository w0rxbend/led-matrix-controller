#pragma once

#include <Arduino.h>

#include "AppConfig.h"

namespace MatrixProtocol {

// The TCP protocol is intentionally tiny:
//
//   'L' 'M' version command payloadLength payload checksum
//
// A fixed header and a 1-byte payload length keep the protocol tiny.
// Some commands need metadata, so the maximum payload is now 255 bytes.
constexpr uint8_t kMagic0 = 0x4C;
constexpr uint8_t kMagic1 = 0x4D;
constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kResponseCommand = 0x80;
constexpr uint16_t kMaxPayloadSize = 255;
constexpr uint16_t kHeaderSize = 5;
constexpr uint16_t kChecksumSize = 1;
constexpr uint16_t kMaxFrameSize = kHeaderSize + kMaxPayloadSize + kChecksumSize;
constexpr uint16_t kResponseSize = 6;

enum class Command : uint8_t {
  // Connectivity check. Does not change the matrix.
  kPing = 0x00,

  // Clears all pixels to black.
  kClear = 0x01,

  // Updates global brightness with one payload byte: 0..255.
  kSetBrightness = 0x02,

  // Fills the matrix with one RGB triple.
  kFill = 0x03,

  // Sets one logical x/y pixel using payload: x, y, r, g, b.
  kSetPixel = 0x04,

  // Replaces the full physical LED buffer using 64 RGB triples.
  kSetFrame = 0x05,

  // Turns visible panel output off/on without clearing the stored image.
  kSetPanelEnabled = 0x06,

  // Sets a single static color and keeps matrix in static mode.
  kSetStaticColor = 0x07,

  // Applies one preset effect.
  kSetPresetEffect = 0x08,

  // Uploads one frame for the single custom animation slot.
  kUploadCustomFrame = 0x09,

  // Stops any running effect and returns control to direct commands.
  kStopEffect = 0x0A,
};

enum class Status : uint8_t {
  // Command was valid and has been applied.
  kOk = 0x00,

  // The first two bytes were not 'L' and 'M'.
  kBadMagic = 0x01,

  // The client is speaking another protocol version.
  kUnsupportedVersion = 0x02,

  // Header was valid, but command id is not implemented.
  kUnknownCommand = 0x03,

  // Command exists, but payload length or coordinate range is wrong.
  kInvalidLength = 0x04,

  // Frame arrived but did not pass the XOR checksum.
  kChecksumMismatch = 0x05,
};

// Simple corruption check for short LAN/AP packets. This is not cryptographic;
// it only catches malformed or truncated frames before applying LED changes.
uint8_t checksum(const uint8_t* data, uint16_t length);

}  // namespace MatrixProtocol
