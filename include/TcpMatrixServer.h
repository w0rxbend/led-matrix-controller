#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "EffectEngine.h"
#include "FrameParser.h"
#include "LedMatrixController.h"
#include "MatrixProtocol.h"

// Coordinates Wi-Fi, TCP transport, protocol parsing, and matrix command
// dispatch.
//
// This class intentionally owns all network state. The main Arduino loop only
// calls loop(), while this class decides whether to reconnect Wi-Fi, restart the
// listener, accept a client, read bytes, and dispatch parsed commands.
class TcpMatrixServer {
 public:
  explicit TcpMatrixServer(LedMatrixController& matrix);

  // Starts Wi-Fi and starts the TCP listener if the network is ready.
  void begin();

  // Must be called repeatedly from Arduino loop(). It never blocks for long.
  void loop();

 private:
  // Network startup and retry helpers.
  void startWifi();
  void beginStationConnect();
  void startAccessPoint();
  void handleWifiReconnect();
  bool hasStationCredentials() const;
  bool networkIsReady() const;
  IPAddress currentIpAddress() const;
  void printNetworkAddress() const;

  // TCP listener lifecycle helpers. Keeping these separate makes failure and
  // reconnect paths explicit.
  void ensureServerRunning();
  void startServer();
  void stopServer();
  void restartServer();
  void acceptClientIfNeeded();
  void readClientBytes();
  void dropIdleClient();

  // Acts on a frame the parser has already validated.
  void processFrame();

  // Converts a validated protocol frame into LED operations.
  MatrixProtocol::Status applyCommand(uint8_t command, const uint8_t* payload, uint8_t length);

  // Sends the compact 6-byte response frame back to the current TCP client.
  void sendStatus(MatrixProtocol::Status status);


  // Matrix is injected so network code does not own LED hardware directly.
  LedMatrixController& matrix_;

  // Effects are a separate concern with no network state of their own.
  EffectEngine effects_;

  // ESP8266 TCP server/client objects. One connected client is enough for this
  // controller and keeps RAM use predictable.
  WiFiServer server_;
  WiFiClient client_;

  // Parser for the in-progress command frame. TCP is a byte stream, so a frame
  // may arrive split across several loop() iterations.
  FrameParser parser_;

  // Retry/health state. millis() timestamps avoid blocking delay loops after
  // setup, which keeps the device responsive.
  bool serverStarted_;
  uint32_t lastWifiRetryMs_;
  uint32_t lastServerHealthCheckMs_;
  uint32_t lastClientActivityMs_;
};
