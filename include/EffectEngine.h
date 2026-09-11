#pragma once

#include <Arduino.h>

#include "AppConfig.h"
#include "LedMatrixController.h"

// Non-blocking animation engine for the panel.
//
// Everything here is driven from update(), which renders at most one frame per
// call and returns immediately -- the firmware has a single loop() and a
// software watchdog, so an effect that blocked would take the network with it.
//
// The engine owns no network state and the server owns no effect state; they
// meet at start(), stop(), applyCustomFrame() and update().
class EffectEngine {
 public:
  enum class Mode : uint8_t {
  kDirect = 0,
  kStatic,
  kChase,
  kColorWipe,
  kBlink,
  kWave,
  kRain,
  kMeteor,
  kRainbow,
  kBreathing,
  kScanner,
  kSparkle,
  kFire,
  kMatrixRain,
  kRipple,
  kTheaterChase,
  kTwinkle,
  kComet,
  kPlasma,
  kDiagonal,
  kBorderChase,
  kHeartbeat,
  kPulseWipe,
  kConfetti,
  kCustom
  };

  explicit EffectEngine(LedMatrixController& matrix);

  // Returns the panel to direct command control, clearing it.
  void stop();

  // Begins an effect. intervalMs is clamped to a rate the panel can sustain.
  void start(Mode mode, uint16_t intervalMs, uint8_t red, uint8_t green, uint8_t blue);

  // Stores one frame of the device-side animation. Playback starts once every
  // frame of the declared count has arrived. Returns false if the frame index,
  // count, or ordering is not one the single animation slot can hold.
  bool applyCustomFrame(uint8_t frameIndex, uint8_t frameCount, uint16_t delayMs,
                        const uint8_t* frameData);

  // Advances the running effect if its interval has elapsed. Call every loop().
  void update();

 private:
  void renderEffectFrame(uint32_t nowMs);
  void renderStatic(uint32_t nowMs);
  void renderChase(uint32_t nowMs);
  void renderColorWipe(uint32_t nowMs);
  void renderBlink(uint32_t nowMs);
  void renderWave(uint32_t nowMs);
  void renderRain(uint32_t nowMs);
  void renderMeteor(uint32_t nowMs);
  void renderRainbow(uint32_t nowMs);
  void renderBreathing(uint32_t nowMs);
  void renderScanner(uint32_t nowMs);
  void renderSparkle(uint32_t nowMs);
  void renderFire(uint32_t nowMs);
  void renderMatrixRain(uint32_t nowMs);
  void renderRipple(uint32_t nowMs);
  void renderTheaterChase(uint32_t nowMs);
  void renderTwinkle(uint32_t nowMs);
  void renderComet(uint32_t nowMs);
  void renderPlasma(uint32_t nowMs);
  void renderDiagonal(uint32_t nowMs);
  void renderBorderChase(uint32_t nowMs);
  void renderHeartbeat(uint32_t nowMs);
  void renderPulseWipe(uint32_t nowMs);
  void renderConfetti(uint32_t nowMs);
  void renderCustom(uint32_t nowMs);

  LedMatrixController& matrix_;

  Mode mode_;
  uint16_t intervalMs_;
  uint32_t lastStepMs_;
  uint8_t phase_;
  uint8_t colorRed_;
  uint8_t colorGreen_;
  uint8_t colorBlue_;
  bool blinkState_;
  uint32_t seed_;

  // Device-side animation: one slot, up to kMaxCustomFrames frames.
  uint8_t customFrameCount_;
  uint8_t customFrameExpectedCount_;
  uint16_t customFrameDelayMs_[AppConfig::kMaxCustomFrames];
  uint8_t customReceivedMask_;
  uint8_t customCurrentFrame_;
  uint8_t customFrameBuffer_[AppConfig::kMaxCustomFrames][AppConfig::kLedCount * 3];
};
