#pragma once

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "AppConfig.h"

// High-level wrapper around Adafruit_NeoPixel.
//
// The rest of the application should talk in matrix concepts: clear the panel,
// fill the panel, set logical x/y pixels, or push a complete RGB frame. This
// class keeps those operations in one place and hides the WS2812B library plus
// the physical serpentine wiring order.
class LedMatrixController {
 public:
  LedMatrixController();

  // Initializes the LED driver, applies the safe default brightness, and clears
  // any pixels that may have latched random data during boot.
  void begin();

  // Turns every LED off and immediately pushes that state to the matrix.
  void clear();

  // Changes the global brightness scale used by Adafruit_NeoPixel.
  void setBrightness(uint8_t brightness);

  // Enables/disables visible output without forgetting the current colors.
  void setEnabled(bool enabled);

  // Sets every LED to one RGB color.
  void fill(uint8_t red, uint8_t green, uint8_t blue);

  // Sets one logical x/y pixel. Returns false if the coordinate is outside the
  // configured matrix dimensions.
  bool setPixel(uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue);

  // Pushes a full physical-order frame. The payload is 64 RGB triples for the
  // current 8x8 matrix, ordered exactly as the LED chain is wired.
  bool setPhysicalFrame(const uint8_t* rgbFrame, uint16_t length);

 private:
  // Converts logical coordinates into the LED chain index. Invalid coordinates
  // return AppConfig::kLedCount, which is outside the valid range.
  uint16_t toPhysicalIndex(uint8_t x, uint8_t y) const;

  // Re-renders the stored desired frame to the physical LEDs. When disabled,
  // this intentionally writes black while keeping frameRgb_ unchanged.
  void render();

  Adafruit_NeoPixel pixels_;
  uint8_t frameRgb_[AppConfig::kLedCount * 3];
  bool enabled_;
};
