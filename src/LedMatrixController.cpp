#include "LedMatrixController.h"

LedMatrixController::LedMatrixController()
    : pixels_(AppConfig::kLedCount, AppConfig::kLedPin, NEO_GRB + NEO_KHZ800),
      frameRgb_(),
      enabled_(true) {}

void LedMatrixController::begin() {
  // begin() prepares the GPIO timing code inside Adafruit_NeoPixel. No pixels
  // are guaranteed to change on the wire until show() is called.
  pixels_.begin();

  // Use a conservative brightness at boot. If a client wants full brightness it
  // can request it explicitly after the power supply is known to be stable.
  pixels_.setBrightness(AppConfig::kDefaultBrightness);

  // Clear immediately so the LEDs do not keep random latched data from reset.
  clear();
}

void LedMatrixController::clear() {
  // Clear means "forget the desired image" and turn every stored RGB value to
  // black. Panel off/on does not call this, so it can preserve the image.
  memset(frameRgb_, 0, sizeof(frameRgb_));
  render();
}

void LedMatrixController::setBrightness(uint8_t brightness) {
  pixels_.setBrightness(brightness);

  // Adafruit_NeoPixel applies brightness during show(), so push the existing
  // pixel buffer again after changing the global scale.
  render();
}

void LedMatrixController::setEnabled(bool enabled) {
  enabled_ = enabled;
  render();
}

void LedMatrixController::fill(uint8_t red, uint8_t green, uint8_t blue) {
  // Update the stored desired frame first. render() decides whether that frame
  // is currently visible or hidden by the panel enabled flag.
  for (uint16_t ledIndex = 0; ledIndex < AppConfig::kLedCount; ledIndex++) {
    const uint16_t offset = ledIndex * 3;
    frameRgb_[offset] = red;
    frameRgb_[offset + 1] = green;
    frameRgb_[offset + 2] = blue;
  }

  render();
}

bool LedMatrixController::setPixel(uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue) {
  // Network clients use logical x/y coordinates; the matrix wiring does not.
  // Translate before touching the physical buffer.
  const uint16_t ledIndex = toPhysicalIndex(x, y);
  if (ledIndex >= AppConfig::kLedCount) {
    return false;
  }

  const uint16_t offset = ledIndex * 3;
  frameRgb_[offset] = red;
  frameRgb_[offset + 1] = green;
  frameRgb_[offset + 2] = blue;
  render();
  return true;
}

bool LedMatrixController::setPhysicalFrame(const uint8_t* rgbFrame, uint16_t length) {
  // A full frame must contain exactly one RGB triple for every physical LED.
  // Rejecting short/long frames prevents reading outside the TCP payload.
  if (length != AppConfig::kLedCount * 3) {
    return false;
  }

  // Full-frame updates are already in physical LED order. This avoids an x/y
  // conversion per pixel for animation clients that stream complete frames.
  memcpy(frameRgb_, rgbFrame, sizeof(frameRgb_));
  render();
  return true;
}

void LedMatrixController::render() {
  for (uint16_t ledIndex = 0; ledIndex < AppConfig::kLedCount; ledIndex++) {
    const uint16_t offset = ledIndex * 3;

    if (!enabled_) {
      pixels_.setPixelColor(ledIndex, 0);
      continue;
    }

    pixels_.setPixelColor(
        ledIndex, pixels_.Color(frameRgb_[offset], frameRgb_[offset + 1], frameRgb_[offset + 2]));
  }

  pixels_.show();
}

uint16_t LedMatrixController::toPhysicalIndex(uint8_t x, uint8_t y) const {
  // Returning an out-of-range index lets callers use one simple validity check.
  if (x >= AppConfig::kMatrixWidth || y >= AppConfig::kMatrixHeight) {
    return AppConfig::kLedCount;
  }

  // The 8x8 panel is wired in serpentine rows:
  //   row 0: left -> right
  //   row 1: right -> left
  //   row 2: left -> right
  // This is common for prebuilt WS2812B matrices.
  if (y % 2 == 0) {
    return y * AppConfig::kMatrixWidth + x;
  }

  return y * AppConfig::kMatrixWidth + (AppConfig::kMatrixWidth - 1 - x);
}
