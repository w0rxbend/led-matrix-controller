#pragma once

#include <stdint.h>

#include "AppConfig.h"

// Physical wiring geometry for the panel.
//
// This is deliberately free of Arduino and Adafruit_NeoPixel dependencies: the
// mapping from logical x/y to LED chain index is pure arithmetic, every visible
// pixel depends on it being right, and keeping it here lets it be unit tested on
// the host instead of only on hardware.
namespace MatrixLayout {

// Index returned for coordinates outside the matrix. It is deliberately out of
// range so callers need only one validity check.
constexpr uint16_t kInvalidIndex = AppConfig::kLedCount;

constexpr bool isInside(uint8_t x, uint8_t y) {
  return x < AppConfig::kMatrixWidth && y < AppConfig::kMatrixHeight;
}

// Converts logical x/y into the LED chain index.
//
// The 8x8 panel is wired in serpentine rows:
//   row 0: left  -> right
//   row 1: right -> left
//   row 2: left  -> right
// which is how prebuilt WS2812B matrices are usually assembled.
constexpr uint16_t toPhysicalIndex(uint8_t x, uint8_t y) {
  if (!isInside(x, y)) {
    return kInvalidIndex;
  }
  if (y % 2 == 0) {
    return static_cast<uint16_t>(y * AppConfig::kMatrixWidth + x);
  }
  return static_cast<uint16_t>(y * AppConfig::kMatrixWidth + (AppConfig::kMatrixWidth - 1 - x));
}

}  // namespace MatrixLayout
