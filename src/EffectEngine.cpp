#include "EffectEngine.h"

#include <cstring>

#include "MatrixLayout.h"

namespace {

// Step tables for the effects that walk a fixed sequence. constexpr because
// nothing writes them and the k-prefix said so already -- note this does not
// move them out of RAM: on the ESP8266 .rodata is loaded into DRAM, and only
// PROGMEM places data in flash, which for 36 bytes is not worth the
// pgm_read_byte indirection at every use.
constexpr uint8_t kColorWaveSteps[8] = {0, 2, 4, 6, 7, 5, 3, 1};
constexpr uint8_t kBreathingSteps[16] = {10, 18, 28, 42, 60, 84, 112, 150, 190, 150, 112, 84, 60, 42, 28, 18};
constexpr uint8_t kHeartbeatSteps[12] = {0, 180, 255, 70, 0, 0, 120, 210, 55, 0, 0, 0};

uint16_t clampDelayMs(uint16_t delayMs) {
  if (delayMs == 0) {
    return AppConfig::kDefaultPresetIntervalMs;
  }
  return delayMs < AppConfig::kMinEffectFrameDelayMs ? AppConfig::kMinEffectFrameDelayMs : delayMs;
}

uint8_t logicalToPhysical(uint8_t x, uint8_t y) {
  if (x >= AppConfig::kMatrixWidth || y >= AppConfig::kMatrixHeight) {
    return AppConfig::kLedCount;
  }

  if (y % 2 == 0) {
    return y * AppConfig::kMatrixWidth + x;
  }

  return y * AppConfig::kMatrixWidth + (AppConfig::kMatrixWidth - 1 - x);
}

uint32_t xorshift32(uint32_t& state) {
  state ^= (state << 13);
  state ^= (state >> 17);
  state ^= (state << 5);
  return state;
}

uint8_t scaled(uint8_t value, uint8_t scale) {
  return static_cast<uint8_t>((static_cast<uint16_t>(value) * scale) / 255);
}

void setFramePixel(uint8_t* frame, uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue) {
  const uint8_t physicalIndex = logicalToPhysical(x, y);
  if (physicalIndex >= AppConfig::kLedCount) {
    return;
  }

  const uint16_t base = static_cast<uint16_t>(physicalIndex) * 3;
  frame[base] = red;
  frame[base + 1] = green;
  frame[base + 2] = blue;
}

bool perimeterToPoint(uint8_t perimeterIndex, uint8_t& x, uint8_t& y) {
  constexpr uint8_t perimeterLength =
      (AppConfig::kMatrixWidth * 2) + (AppConfig::kMatrixHeight * 2) - 4;
  perimeterIndex %= perimeterLength;

  if (perimeterIndex < AppConfig::kMatrixWidth) {
    x = perimeterIndex;
    y = 0;
    return true;
  }

  perimeterIndex -= AppConfig::kMatrixWidth;
  if (perimeterIndex < AppConfig::kMatrixHeight - 1) {
    x = AppConfig::kMatrixWidth - 1;
    y = perimeterIndex + 1;
    return true;
  }

  perimeterIndex -= AppConfig::kMatrixHeight - 1;
  if (perimeterIndex < AppConfig::kMatrixWidth - 1) {
    x = AppConfig::kMatrixWidth - 2 - perimeterIndex;
    y = AppConfig::kMatrixHeight - 1;
    return true;
  }

  perimeterIndex -= AppConfig::kMatrixWidth - 1;
  x = 0;
  y = AppConfig::kMatrixHeight - 2 - perimeterIndex;
  return true;
}

void wheelColor(uint8_t position, uint8_t& red, uint8_t& green, uint8_t& blue) {
  position = 255 - position;
  if (position < 85) {
    red = 255 - position * 3;
    green = 0;
    blue = position * 3;
    return;
  }

  if (position < 170) {
    position -= 85;
    red = 0;
    green = position * 3;
    blue = 255 - position * 3;
    return;
  }

  position -= 170;
  red = position * 3;
  green = 255 - position * 3;
  blue = 0;
}
}  // namespace

EffectEngine::EffectEngine(LedMatrixController& matrix)
    : matrix_(matrix),
      mode_(Mode::kDirect),
      intervalMs_(AppConfig::kDefaultPresetIntervalMs),
      lastStepMs_(0),
      phase_(0),
      colorRed_(255),
      colorGreen_(255),
      colorBlue_(255),
      blinkState_(false),
      seed_(0xDEADBEEF),
      customFrameCount_(0),
      customFrameExpectedCount_(0),
      customReceivedMask_(0),
      customCurrentFrame_(0) {
  memset(customFrameDelayMs_, 0, sizeof(customFrameDelayMs_));
  memset(customFrameBuffer_, 0, sizeof(customFrameBuffer_));
}

void EffectEngine::stop() {
  mode_ = Mode::kDirect;
  phase_ = 0;
  blinkState_ = false;
  customFrameCount_ = 0;
  customFrameExpectedCount_ = 0;
  customReceivedMask_ = 0;
}

void EffectEngine::start(Mode mode, uint16_t intervalMs, uint8_t red, uint8_t green,
                         uint8_t blue) {
  mode_ = mode;
  // Clamped here rather than by each caller: a frame interval the panel cannot
  // sustain is the engine's invariant to keep, not the protocol layer's.
  intervalMs_ = clampDelayMs(intervalMs);
  colorRed_ = red;
  colorGreen_ = green;
  colorBlue_ = blue;
  phase_ = 0;
  blinkState_ = false;
  lastStepMs_ = 0;

  renderEffectFrame(millis());
}

bool EffectEngine::applyCustomFrame(uint8_t frameIndex, uint8_t frameCount, uint16_t delayMs,
                                       const uint8_t* frameData) {
  if (frameCount == 0 || frameCount > AppConfig::kMaxCustomFrames) {
    return false;
  }

  if (frameIndex >= frameCount) {
    return false;
  }

  if (customFrameExpectedCount_ != frameCount || frameIndex == 0) {
    memset(customFrameDelayMs_, 0, sizeof(customFrameDelayMs_));
    memset(customFrameBuffer_, 0, sizeof(customFrameBuffer_));
    customFrameCount_ = 0;
    customFrameExpectedCount_ = frameCount;
    customReceivedMask_ = 0;
    customCurrentFrame_ = 0;
  }

  const uint8_t bit = static_cast<uint8_t>(1 << frameIndex);
  const bool newFrame = (customReceivedMask_ & bit) == 0;
  memcpy(customFrameBuffer_[frameIndex], frameData, AppConfig::kLedCount * 3);
  customFrameDelayMs_[frameIndex] = clampDelayMs(delayMs);
  if (newFrame) {
    customFrameCount_++;
  }
  customReceivedMask_ |= bit;

  if (customFrameCount_ == customFrameExpectedCount_) {
    start(Mode::kCustom, AppConfig::kDefaultPresetIntervalMs, 255, 255, 255);
    renderCustom(millis());
  }
  return true;
}

void EffectEngine::update() {
  if (mode_ == Mode::kDirect) {
    return;
  }

  const uint32_t nowMs = millis();
  renderEffectFrame(nowMs);
}

void EffectEngine::renderEffectFrame(uint32_t nowMs) {
  if (mode_ == Mode::kCustom) {
    renderCustom(nowMs);
    return;
  }

  if ((intervalMs_ == 0) || (nowMs - lastStepMs_ < intervalMs_)) {
    return;
  }

  lastStepMs_ = nowMs;
  phase_++;

  switch (mode_) {
    case Mode::kStatic:
      renderStatic(nowMs);
      break;
    case Mode::kChase:
      renderChase(nowMs);
      break;
    case Mode::kColorWipe:
      renderColorWipe(nowMs);
      break;
    case Mode::kBlink:
      renderBlink(nowMs);
      break;
    case Mode::kWave:
      renderWave(nowMs);
      break;
    case Mode::kRain:
      renderRain(nowMs);
      break;
    case Mode::kMeteor:
      renderMeteor(nowMs);
      break;
    case Mode::kRainbow:
      renderRainbow(nowMs);
      break;
    case Mode::kBreathing:
      renderBreathing(nowMs);
      break;
    case Mode::kScanner:
      renderScanner(nowMs);
      break;
    case Mode::kSparkle:
      renderSparkle(nowMs);
      break;
    case Mode::kFire:
      renderFire(nowMs);
      break;
    case Mode::kMatrixRain:
      renderMatrixRain(nowMs);
      break;
    case Mode::kRipple:
      renderRipple(nowMs);
      break;
    case Mode::kTheaterChase:
      renderTheaterChase(nowMs);
      break;
    case Mode::kTwinkle:
      renderTwinkle(nowMs);
      break;
    case Mode::kComet:
      renderComet(nowMs);
      break;
    case Mode::kPlasma:
      renderPlasma(nowMs);
      break;
    case Mode::kDiagonal:
      renderDiagonal(nowMs);
      break;
    case Mode::kBorderChase:
      renderBorderChase(nowMs);
      break;
    case Mode::kHeartbeat:
      renderHeartbeat(nowMs);
      break;
    case Mode::kPulseWipe:
      renderPulseWipe(nowMs);
      break;
    case Mode::kConfetti:
      renderConfetti(nowMs);
      break;
    default:
      break;
  }
}

void EffectEngine::renderStatic(uint32_t /*nowMs*/) {
  matrix_.fill(colorRed_, colorGreen_, colorBlue_);
}

void EffectEngine::renderChase(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint16_t ledIndex = static_cast<uint16_t>(phase_ % AppConfig::kLedCount);
  const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
  const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
  const uint8_t physicalIndex = logicalToPhysical(x, y);

  if (physicalIndex < AppConfig::kLedCount) {
    const uint16_t base = physicalIndex * 3;
    frame[base] = colorRed_;
    frame[base + 1] = colorGreen_;
    frame[base + 2] = colorBlue_;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderColorWipe(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t targetCount = static_cast<uint8_t>(phase_ % (AppConfig::kLedCount + 1));

  for (uint8_t ledIndex = 0; ledIndex < targetCount; ++ledIndex) {
    const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
    const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
    const uint8_t physicalIndex = logicalToPhysical(x, y);
    const uint16_t base = static_cast<uint16_t>(physicalIndex) * 3;
    frame[base] = colorRed_;
    frame[base + 1] = colorGreen_;
    frame[base + 2] = colorBlue_;
  }

  if (targetCount == AppConfig::kLedCount) {
    phase_ = 0;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderBlink(uint32_t /*nowMs*/) {
  blinkState_ = !blinkState_;
  if (blinkState_) {
    matrix_.fill(colorRed_, colorGreen_, colorBlue_);
  } else {
    matrix_.clear();
  }
}

void EffectEngine::renderWave(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t wave = kColorWaveSteps[(x + phase_ + y) % 8];
      const uint16_t base = static_cast<uint16_t>(logicalToPhysical(x, y)) * 3;
      frame[base] = static_cast<uint8_t>((static_cast<uint16_t>(colorRed_) * wave) / 7);
      frame[base + 1] = static_cast<uint8_t>((static_cast<uint16_t>(colorGreen_) * wave) / 7);
      frame[base + 2] = static_cast<uint8_t>((static_cast<uint16_t>(colorBlue_) * wave) / 7);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderRain(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
    const uint8_t spawn = static_cast<uint8_t>(xorshift32(seed_) % 100);
    if (spawn < 20) {
      const uint8_t dropY =
          static_cast<uint8_t>(xorshift32(seed_) % AppConfig::kMatrixHeight);
      const uint8_t base = static_cast<uint16_t>(logicalToPhysical(x, dropY)) * 3;
      frame[base] = colorRed_;
      frame[base + 1] = colorGreen_;
      frame[base + 2] = colorBlue_;
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderMeteor(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t tailLength = 4;

  for (uint8_t tail = 0; tail < tailLength; ++tail) {
    const int16_t step = static_cast<int16_t>(phase_ - tail);
    if (step < 0) {
      continue;
    }

    const uint16_t ledIndex = static_cast<uint16_t>(step) % AppConfig::kLedCount;
    const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
    const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
    const uint8_t physicalIndex = logicalToPhysical(x, y);
    if (physicalIndex >= AppConfig::kLedCount) {
      continue;
    }

    const uint16_t base = static_cast<uint16_t>(physicalIndex) * 3;
    const uint16_t scale = static_cast<uint16_t>(tailLength - tail) * 192 / tailLength;
    frame[base] = static_cast<uint8_t>((static_cast<uint16_t>(colorRed_) * scale) / 192);
    frame[base + 1] =
        static_cast<uint8_t>((static_cast<uint16_t>(colorGreen_) * scale) / 192);
    frame[base + 2] = static_cast<uint8_t>((static_cast<uint16_t>(colorBlue_) * scale) / 192);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderRainbow(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      uint8_t red = 0;
      uint8_t green = 0;
      uint8_t blue = 0;
      wheelColor(static_cast<uint8_t>(phase_ * 5 + x * 18 + y * 9), red, green, blue);
      setFramePixel(frame, x, y, red, green, blue);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderBreathing(uint32_t /*nowMs*/) {
  const uint8_t scale = kBreathingSteps[phase_ % 16];
  matrix_.fill(scaled(colorRed_, scale), scaled(colorGreen_, scale),
               scaled(colorBlue_, scale));
}

void EffectEngine::renderScanner(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t span = (AppConfig::kMatrixWidth * 2) - 2;
  const uint8_t position = phase_ % span;
  const uint8_t scannerX = position < AppConfig::kMatrixWidth ? position : span - position;

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t distance = x > scannerX ? x - scannerX : scannerX - x;
      if (distance > 3) {
        continue;
      }

      const uint8_t scale = distance == 0 ? 255 : distance == 1 ? 120 : distance == 2 ? 45 : 12;
      setFramePixel(frame, x, y, scaled(colorRed_, scale), scaled(colorGreen_, scale),
                    scaled(colorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderSparkle(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t spark = 0; spark < 7; ++spark) {
    const uint8_t ledIndex = static_cast<uint8_t>(xorshift32(seed_) % AppConfig::kLedCount);
    const uint8_t scale = spark < 2 ? 255 : 80;
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = scaled(colorRed_, scale);
    frame[base + 1] = scaled(colorGreen_, scale);
    frame[base + 2] = scaled(colorBlue_, scale);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderFire(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    const uint8_t heightHeat = static_cast<uint8_t>(255 - (y * 28));
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t noise = static_cast<uint8_t>(xorshift32(seed_) % 85);
      const uint8_t heat = heightHeat > noise ? heightHeat - noise : 0;
      const uint8_t red = heat;
      const uint8_t green = heat > 128 ? static_cast<uint8_t>((heat - 128) * 2) : heat / 3;
      const uint8_t blue = heat > 230 ? static_cast<uint8_t>((heat - 230) * 4) : 0;
      setFramePixel(frame, x, AppConfig::kMatrixHeight - 1 - y, red, green, blue);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderMatrixRain(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
    const uint8_t head =
        static_cast<uint8_t>((phase_ + x * 3) % (AppConfig::kMatrixHeight + 4));
    for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
      if (head < y || head - y > 3) {
        continue;
      }

      const uint8_t distance = head - y;
      const uint8_t scale = distance == 0 ? 255 : distance == 1 ? 120 : distance == 2 ? 50 : 16;
      setFramePixel(frame, x, y, scaled(colorRed_, scale), scaled(colorGreen_, scale),
                    scaled(colorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderRipple(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t radius = phase_ % 8;

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const int8_t dx = static_cast<int8_t>(x) - 3;
      const int8_t dy = static_cast<int8_t>(y) - 3;
      const uint8_t distance = static_cast<uint8_t>((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
      const uint8_t delta = distance > radius ? distance - radius : radius - distance;
      if (delta > 2) {
        continue;
      }

      const uint8_t scale = delta == 0 ? 255 : delta == 1 ? 90 : 24;
      setFramePixel(frame, x, y, scaled(colorRed_, scale), scaled(colorGreen_, scale),
                    scaled(colorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderTheaterChase(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t phase = phase_ % 3;

  for (uint8_t ledIndex = 0; ledIndex < AppConfig::kLedCount; ++ledIndex) {
    if ((ledIndex + phase) % 3 != 0) {
      continue;
    }

    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = colorRed_;
    frame[base + 1] = colorGreen_;
    frame[base + 2] = colorBlue_;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderTwinkle(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t sparkle = 0; sparkle < 10; ++sparkle) {
    const uint8_t ledIndex = static_cast<uint8_t>(xorshift32(seed_) % AppConfig::kLedCount);
    const uint8_t scale = static_cast<uint8_t>(30 + (xorshift32(seed_) % 226));
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = scaled(colorRed_, scale);
    frame[base + 1] = scaled(colorGreen_, scale);
    frame[base + 2] = scaled(colorBlue_, scale);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderComet(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  constexpr uint8_t tailLength = 8;
  const uint8_t head = phase_ % AppConfig::kLedCount;

  for (uint8_t tail = 0; tail < tailLength; ++tail) {
    const uint8_t ledIndex =
        static_cast<uint8_t>((head + AppConfig::kLedCount - tail) % AppConfig::kLedCount);
    const uint8_t scale = static_cast<uint8_t>(255 - (tail * 28));
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = scaled(colorRed_, scale);
    frame[base + 1] = scaled(colorGreen_, scale);
    frame[base + 2] = scaled(colorBlue_, scale);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderPlasma(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      uint8_t red = 0;
      uint8_t green = 0;
      uint8_t blue = 0;
      const uint8_t position =
          static_cast<uint8_t>((x * x * 9) + (y * y * 7) + (x * y * 5) + (phase_ * 6));
      wheelColor(position, red, green, blue);
      setFramePixel(frame, x, y, red, green, blue);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderDiagonal(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t band = (x + y + phase_) % 6;
      if (band > 2) {
        continue;
      }

      const uint8_t scale = band == 0 ? 255 : band == 1 ? 110 : 35;
      setFramePixel(frame, x, y, scaled(colorRed_, scale), scaled(colorGreen_, scale),
                    scaled(colorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderBorderChase(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  constexpr uint8_t perimeterLength =
      (AppConfig::kMatrixWidth * 2) + (AppConfig::kMatrixHeight * 2) - 4;

  for (uint8_t tail = 0; tail < 6; ++tail) {
    uint8_t x = 0;
    uint8_t y = 0;
    perimeterToPoint(static_cast<uint8_t>(phase_ + perimeterLength - tail), x, y);
    const uint8_t scale = static_cast<uint8_t>(255 - (tail * 35));
    setFramePixel(frame, x, y, scaled(colorRed_, scale), scaled(colorGreen_, scale),
                  scaled(colorBlue_, scale));
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderHeartbeat(uint32_t /*nowMs*/) {
  const uint8_t scale = kHeartbeatSteps[phase_ % 12];
  matrix_.fill(scaled(colorRed_, scale), scaled(colorGreen_, scale),
               scaled(colorBlue_, scale));
}

void EffectEngine::renderPulseWipe(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t scale = kBreathingSteps[phase_ % 16];
  const uint8_t targetCount = static_cast<uint8_t>(phase_ % (AppConfig::kLedCount + 1));

  for (uint8_t ledIndex = 0; ledIndex < targetCount; ++ledIndex) {
    const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
    const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
    setFramePixel(frame, x, y, scaled(colorRed_, scale), scaled(colorGreen_, scale),
                  scaled(colorBlue_, scale));
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderConfetti(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t dot = 0; dot < 9; ++dot) {
    const uint8_t ledIndex = static_cast<uint8_t>(xorshift32(seed_) % AppConfig::kLedCount);
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    wheelColor(static_cast<uint8_t>(phase_ * 11 + ledIndex * 17 + dot * 23), red, green,
               blue);
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = red;
    frame[base + 1] = green;
    frame[base + 2] = blue;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void EffectEngine::renderCustom(uint32_t nowMs) {
  if (customFrameExpectedCount_ == 0 || customFrameCount_ < customFrameExpectedCount_) {
    return;
  }

  const uint16_t delayMs = customFrameDelayMs_[customCurrentFrame_] == 0
                               ? AppConfig::kDefaultPresetIntervalMs
                               : customFrameDelayMs_[customCurrentFrame_];
  if (nowMs - lastStepMs_ < delayMs) {
    return;
  }

  lastStepMs_ = nowMs;
  matrix_.setPhysicalFrame(customFrameBuffer_[customCurrentFrame_], AppConfig::kLedCount * 3);
  customCurrentFrame_ = (customCurrentFrame_ + 1) % customFrameExpectedCount_;
}
