#include <Arduino.h>

#include "AppConfig.h"
#include "LedMatrixController.h"
#include "TcpMatrixServer.h"

LedMatrixController ledMatrix;
TcpMatrixServer tcpServer(ledMatrix);

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
  tcpServer.begin();
}

void loop() {
  // All retry/reconnect/parser work is inside TcpMatrixServer. Keeping loop()
  // this small makes it obvious there are no hidden blocking animations here.
  tcpServer.loop();
}
