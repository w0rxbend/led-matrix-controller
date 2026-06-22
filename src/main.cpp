#include <Arduino.h>

#include "AppConfig.h"
#include "LedMatrixController.h"
#include "TcpMatrixServer.h"

LedMatrixController ledMatrix;
TcpMatrixServer tcpServer(ledMatrix);

namespace {

void runStartupAnimation() {
  // Give a short visual indicator that firmware boot finished and matrix control is
  // live. The animation is intentionally simple and bounded to avoid long startup
  // delays.
  Serial.println("Startup animation: running");

  ledMatrix.clear();
  delay(100);

  // Pixel sweep: one bright white pixel moves across the panel a few times.
  constexpr uint8_t sweepRepeats = 2;
  constexpr uint16_t sweepDelayMs = 35;

  for (uint8_t repeat = 0; repeat < sweepRepeats; ++repeat) {
    for (uint8_t logicalIndex = 0; logicalIndex < AppConfig::kLedCount; ++logicalIndex) {
      const uint8_t x = logicalIndex % AppConfig::kMatrixWidth;
      const uint8_t y = logicalIndex / AppConfig::kMatrixWidth;

      ledMatrix.clear();
      ledMatrix.setPixel(x, y, 255, 255, 255);
      delay(sweepDelayMs);
    }
  }

  // Confirm success state with a short green flash, then leave the panel on so the
  // user can immediately see it is ready.
  ledMatrix.fill(0, 255, 0);
  delay(180);
  ledMatrix.clear();
  delay(80);
}

}  // namespace

void setup() {
  // Serial is only for diagnostics. The controller protocol itself is TCP.
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("ESP8266 WS2812B TCP matrix controller");
  Serial.println("LED data pin: D2 / GPIO4");
  Serial.print("Boot settle delay ms: ");
  Serial.println(AppConfig::kBootSettleDelayMs);

  // WS2812B panels and ESP8266 Wi-Fi both create startup current spikes. This
  // pause gives external supplies and USB power time to settle before use.
  delay(AppConfig::kBootSettleDelayMs);

  // Initialize hardware first, then networking. If Wi-Fi takes time to connect,
  // the LED matrix is already in a known cleared state.
  ledMatrix.begin();
  runStartupAnimation();
  tcpServer.begin();
}

void loop() {
  // All retry/reconnect/parser work is inside TcpMatrixServer. Keeping loop()
  // this small makes it obvious there are no hidden blocking animations here.
  tcpServer.loop();
}
