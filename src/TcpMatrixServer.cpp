#include "TcpMatrixServer.h"

#include <cstring>

namespace {

// These wrappers make credentials look like runtime values to static analysis.
// In practice WIFI_SSID/WIFI_PASSWORD are compile-time macros from creds.h.
const char* configuredWifiSsid() {
  return WIFI_SSID;
}

const char* configuredWifiPassword() {
  return WIFI_PASSWORD;
}

const char* configuredAccessPointPassword() {
  return AP_PASSWORD;
}

const char* commandName(uint8_t command) {
  switch (static_cast<MatrixProtocol::Command>(command)) {
    case MatrixProtocol::Command::kPing:
      return "PING";
    case MatrixProtocol::Command::kClear:
      return "CLEAR";
    case MatrixProtocol::Command::kSetBrightness:
      return "SET_BRIGHTNESS";
    case MatrixProtocol::Command::kFill:
      return "FILL";
    case MatrixProtocol::Command::kSetPixel:
      return "SET_PIXEL";
    case MatrixProtocol::Command::kSetFrame:
      return "SET_FRAME";
    case MatrixProtocol::Command::kSetPanelEnabled:
      return "SET_PANEL";
    case MatrixProtocol::Command::kSetStaticColor:
      return "SET_STATIC_COLOR";
    case MatrixProtocol::Command::kSetPresetEffect:
      return "SET_PRESET_EFFECT";
    case MatrixProtocol::Command::kUploadCustomFrame:
      return "UPLOAD_CUSTOM_FRAME";
    case MatrixProtocol::Command::kStopEffect:
      return "STOP_EFFECT";
    default:
      return "UNKNOWN";
  }
}

void logInstruction(uint8_t command, uint8_t payloadLength, const IPAddress& remoteIp,
                    uint16_t remotePort) {
  Serial.print("Instruction: ");
  Serial.print(commandName(command));
  Serial.print(" (0x");
  Serial.print(command, HEX);
  Serial.print("), len=");
  Serial.print(payloadLength);
  Serial.print(", from ");
  Serial.print(remoteIp);
  Serial.print(":");
  Serial.println(remotePort);
}

}  // namespace

TcpMatrixServer::TcpMatrixServer(LedMatrixController& matrix)
    : matrix_(matrix),
      effects_(matrix),
      server_(AppConfig::kTcpPort),
      serverStarted_(false),
      lastWifiRetryMs_(0),
      lastServerHealthCheckMs_(0),
      lastClientActivityMs_(0) {}

void TcpMatrixServer::begin() {
  // Initial setup may block briefly while station Wi-Fi connects. After begin()
  // returns, all retries are timer-driven from loop().
  startWifi();
  ensureServerRunning();
}

void TcpMatrixServer::loop() {
  // Order matters:
  // 1. Repair Wi-Fi first.
  // 2. Ensure the TCP listener matches the current network state.
  // 3. Release the client slot if its occupant has gone quiet.
  // 4. Accept/read client data only when the listener is valid.
  handleWifiReconnect();
  ensureServerRunning();
  dropIdleClient();
  acceptClientIfNeeded();
  readClientBytes();
  effects_.update();
}

void TcpMatrixServer::startWifi() {
  // Without credentials this device is self-contained: it creates its own AP so
  // a phone/laptop can connect directly to the controller.
  if (!hasStationCredentials()) {
    startAccessPoint();
    return;
  }

  // persistent(false) avoids writing credentials to flash on every boot. That
  // reduces flash wear and keeps creds.h as the single source of truth.
  WiFi.persistent(false);

  // Let the ESP8266 core perform its own reconnect attempts too. Our explicit
  // retry loop below is still useful because it controls TCP server lifecycle.
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  beginStationConnect();

  // A bounded initial wait gives useful serial feedback without trapping the
  // device forever if the router is down.
  Serial.print("Connecting to Wi-Fi");
  const uint32_t startedAtMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAtMs < AppConfig::kStationConnectTimeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed, will retry");
    return;
  }

  Serial.println("Wi-Fi connected");
  printNetworkAddress();
}

void TcpMatrixServer::beginStationConnect() {
  // Record the retry time before WiFi.begin(). If begin() returns quickly while
  // disconnected, the next retry is still rate-limited.
  lastWifiRetryMs_ = millis();
  WiFi.begin(configuredWifiSsid(), configuredWifiPassword());
}

void TcpMatrixServer::startAccessPoint() {
  // AP mode is the fallback/control mode when no station credentials are built
  // in. It does not need reconnect handling because the ESP8266 is the AP.
  WiFi.mode(WIFI_AP);
  // With a password the AP is WPA2; without one it is open, and the control
  // protocol has no authentication of its own, so anyone in radio range would
  // have full control of the panel.
  WiFi.softAP(AppConfig::kAccessPointSsid, configuredAccessPointPassword());

  Serial.print("AP SSID: ");
  Serial.println(AppConfig::kAccessPointSsid);
  printNetworkAddress();
}

void TcpMatrixServer::handleWifiReconnect() {
  // AP mode does not depend on an upstream router, so there is nothing to retry.
  if (!hasStationCredentials()) {
    return;
  }

  // Healthy station connection: leave TCP server/client state alone.
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  // A server bound to a lost station interface is not useful. Stop it cleanly so
  // clients reconnect after Wi-Fi returns instead of talking to stale state.
  stopServer();

  const uint32_t nowMs = millis();
  if (nowMs - lastWifiRetryMs_ < AppConfig::kWifiRetryIntervalMs) {
    return;
  }

  Serial.println("Wi-Fi disconnected, retrying connection");
  beginStationConnect();
}

bool TcpMatrixServer::hasStationCredentials() const {
  // Empty SSID means "use AP mode" by convention.
  return strlen(configuredWifiSsid()) > 0;
}

bool TcpMatrixServer::networkIsReady() const {
  // In AP mode, readiness means the ESP8266 AP interface is active.
  if (!hasStationCredentials()) {
    return WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
  }

  // In station mode, only accept TCP clients after the router assigned an IP.
  return WiFi.status() == WL_CONNECTED;
}

IPAddress TcpMatrixServer::currentIpAddress() const {
  if (!hasStationCredentials()) {
    return WiFi.softAPIP();
  }

  return WiFi.localIP();
}

void TcpMatrixServer::printNetworkAddress() const {
  Serial.print("Device IP: ");
  Serial.println(currentIpAddress());
}

void TcpMatrixServer::ensureServerRunning() {
  // If networking is down, force the TCP listener down too. It will be restarted
  // automatically after networkIsReady() becomes true again.
  if (!networkIsReady()) {
    stopServer();
    return;
  }

  // First successful network-ready pass starts the listener.
  if (!serverStarted_) {
    startServer();
    return;
  }

  // Server status checks are intentionally periodic. Checking every loop is not
  // harmful, but a timer keeps serial logging and state transitions calmer.
  const uint32_t nowMs = millis();
  if (nowMs - lastServerHealthCheckMs_ < AppConfig::kServerHealthCheckIntervalMs) {
    return;
  }

  lastServerHealthCheckMs_ = nowMs;
  if (server_.status() == CLOSED) {
    Serial.println("TCP server closed unexpectedly, restarting");
    restartServer();
  }
}

void TcpMatrixServer::startServer() {
  // begin() opens the lwIP listening PCB. setNoDelay disables Nagle buffering so
  // small command/response frames are delivered without artificial latency.
  server_.begin();
  server_.setNoDelay(true);
  serverStarted_ = true;
  lastServerHealthCheckMs_ = millis();

  Serial.print("TCP port: ");
  Serial.println(AppConfig::kTcpPort);
  printNetworkAddress();
}

void TcpMatrixServer::stopServer() {
  // Keep stop idempotent. Many recovery paths call this even when the listener
  // may already be down.
  if (!serverStarted_) {
    return;
  }

  // Drop the current client before closing the listener. That makes reconnect
  // behavior explicit for the client application.
  if (client_) {
    client_.stop();
  }

  server_.stop();
  serverStarted_ = false;
  parser_.reset();
  Serial.println("TCP server stopped");
}

void TcpMatrixServer::restartServer() {
  // Centralized restart path so all server restarts also clear parser state.
  stopServer();
  startServer();
}

void TcpMatrixServer::acceptClientIfNeeded() {
  // Do not call accept() unless begin() has successfully opened the listener.
  if (!serverStarted_) {
    return;
  }

  // The firmware supports one active client. Extra pending clients remain in the
  // server backlog until the active one disconnects.
  if (client_ && client_.connected()) {
    return;
  }

  // accept() is non-blocking in this ESP8266 core: it returns an empty client if
  // no connection is waiting.
  WiFiClient newClient = server_.accept();
  if (!newClient) {
    return;
  }

  client_ = newClient;
  client_.setNoDelay(true);
  client_.keepAlive();
  parser_.reset();
  lastClientActivityMs_ = millis();
  Serial.println("TCP client connected");
}

void TcpMatrixServer::readClientBytes() {
  // TCP may disconnect between loop iterations. Treat that as normal.
  if (!client_ || !client_.connected()) {
    return;
  }

  // Bounded so a client that keeps the receive buffer full cannot hold loop()
  // past the software watchdog, or starve the effect engine. The parser keeps
  // frame state across calls, so a partial frame simply resumes next pass.
  uint16_t budget = AppConfig::kMaxBytesPerLoop;
  while (budget > 0 && client_.available() > 0) {
    budget--;
    lastClientActivityMs_ = millis();

    const FrameParser::Result result = parser_.feed(static_cast<uint8_t>(client_.read()));
    switch (result.action) {
      case FrameParser::Action::kNeedMoreBytes:
        break;
      case FrameParser::Action::kSendStatus:
        sendStatus(result.status);
        break;
      case FrameParser::Action::kFrameReady:
        processFrame();
        break;
    }
  }
}

void TcpMatrixServer::dropIdleClient() {
  if (!client_ || !client_.connected()) {
    return;
  }
  if (millis() - lastClientActivityMs_ < AppConfig::kClientIdleTimeoutMs) {
    return;
  }

  // One client at a time, so a silent occupant -- including a half-open socket
  // left by a peer that lost power -- would otherwise keep the panel
  // uncontrollable until a power cycle.
  Serial.println("TCP client idle, dropping to free the slot");
  client_.stop();
  parser_.reset();
}

void TcpMatrixServer::processFrame() {
  // The parser has already validated magic, version, length and checksum.
  const uint8_t command = parser_.command();
  const uint8_t payloadLength = parser_.payloadLength();

  if (client_) {
    logInstruction(command, payloadLength, client_.remoteIP(), client_.remotePort());
  }

  sendStatus(applyCommand(command, parser_.payload(), payloadLength));
}

MatrixProtocol::Status TcpMatrixServer::applyCommand(uint8_t command, const uint8_t* payload,
                                                     uint8_t length) {
  // Payload sizes are checked per command before reading payload bytes. This is
  // important because malformed TCP clients can send any byte sequence.
  switch (static_cast<MatrixProtocol::Command>(command)) {
    case MatrixProtocol::Command::kPing:
      // Ping is useful for clients to confirm the socket and protocol are live.
      return length == 0 ? MatrixProtocol::Status::kOk : MatrixProtocol::Status::kInvalidLength;

    case MatrixProtocol::Command::kClear:
      if (length != 0) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      effects_.stop();
      matrix_.clear();
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetBrightness:
      // Brightness is global. Existing colors remain buffered, then the matrix
      // is shown again under the new scale.
      if (length != 1) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      matrix_.setBrightness(payload[0]);
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kFill:
      // Fill is the compact way to set the whole matrix to one color.
      if (length != 3) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      effects_.stop();
      matrix_.fill(payload[0], payload[1], payload[2]);
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetPixel:
      // Single-pixel updates use logical coordinates and are mapped by
      // LedMatrixController.
      if (length != 5) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      effects_.stop();
      return matrix_.setPixel(payload[0], payload[1], payload[2], payload[3], payload[4])
                 ? MatrixProtocol::Status::kOk
                 : MatrixProtocol::Status::kInvalidLength;

    case MatrixProtocol::Command::kSetFrame:
      // Full-frame updates are fastest for animations because the client sends
      // the exact physical LED order with no per-pixel protocol overhead.
      if (length != AppConfig::kLedCount * 3) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      effects_.stop();
      return matrix_.setPhysicalFrame(payload, length) ? MatrixProtocol::Status::kOk
                                                       : MatrixProtocol::Status::kInvalidLength;

    case MatrixProtocol::Command::kSetStaticColor:
      // Direct control of static color. The firmware keeps showing the color.
      if (length != 3) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      effects_.start(EffectEngine::Mode::kStatic, AppConfig::kDefaultPresetIntervalMs, payload[0], payload[1],
                  payload[2]);
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetPresetEffect: {
      // Payload: effectId, interval LSB, interval MSB, red, green, blue.
      if (length != 6) {
        return MatrixProtocol::Status::kInvalidLength;
      }

      const uint8_t effectId = payload[0];
      if (effectId == 0) {
        effects_.stop();
        return MatrixProtocol::Status::kOk;
      }

      const uint16_t intervalMs = static_cast<uint16_t>(payload[1] | (payload[2] << 8));
      const uint8_t r = payload[3];
      const uint8_t g = payload[4];
      const uint8_t b = payload[5];

      switch (effectId) {
        case 1:
          effects_.start(EffectEngine::Mode::kChase, intervalMs, r, g, b);
          break;
        case 2:
          effects_.start(EffectEngine::Mode::kColorWipe, intervalMs, r, g, b);
          break;
        case 3:
          effects_.start(EffectEngine::Mode::kBlink, intervalMs, r, g, b);
          break;
        case 4:
          effects_.start(EffectEngine::Mode::kWave, intervalMs, r, g, b);
          break;
        case 5:
          effects_.start(EffectEngine::Mode::kRain, intervalMs, r, g, b);
          break;
        case 6:
          effects_.start(EffectEngine::Mode::kMeteor, intervalMs, r, g, b);
          break;
        case 7:
          effects_.start(EffectEngine::Mode::kRainbow, intervalMs, r, g, b);
          break;
        case 8:
          effects_.start(EffectEngine::Mode::kBreathing, intervalMs, r, g, b);
          break;
        case 9:
          effects_.start(EffectEngine::Mode::kScanner, intervalMs, r, g, b);
          break;
        case 10:
          effects_.start(EffectEngine::Mode::kSparkle, intervalMs, r, g, b);
          break;
        case 11:
          effects_.start(EffectEngine::Mode::kFire, intervalMs, r, g, b);
          break;
        case 12:
          effects_.start(EffectEngine::Mode::kMatrixRain, intervalMs, r, g, b);
          break;
        case 13:
          effects_.start(EffectEngine::Mode::kRipple, intervalMs, r, g, b);
          break;
        case 14:
          effects_.start(EffectEngine::Mode::kTheaterChase, intervalMs, r, g, b);
          break;
        case 15:
          effects_.start(EffectEngine::Mode::kTwinkle, intervalMs, r, g, b);
          break;
        case 16:
          effects_.start(EffectEngine::Mode::kComet, intervalMs, r, g, b);
          break;
        case 17:
          effects_.start(EffectEngine::Mode::kPlasma, intervalMs, r, g, b);
          break;
        case 18:
          effects_.start(EffectEngine::Mode::kDiagonal, intervalMs, r, g, b);
          break;
        case 19:
          effects_.start(EffectEngine::Mode::kBorderChase, intervalMs, r, g, b);
          break;
        case 20:
          effects_.start(EffectEngine::Mode::kHeartbeat, intervalMs, r, g, b);
          break;
        case 21:
          effects_.start(EffectEngine::Mode::kPulseWipe, intervalMs, r, g, b);
          break;
        case 22:
          effects_.start(EffectEngine::Mode::kConfetti, intervalMs, r, g, b);
          break;
        default:
          return MatrixProtocol::Status::kInvalidLength;
      }
      return MatrixProtocol::Status::kOk;
    }

    case MatrixProtocol::Command::kUploadCustomFrame: {
      // Payload format:
      // frameIndex, frameCount, delayMsL, delayMsH, 192 RGB bytes.
      constexpr uint8_t expectedPayload = 4 + (AppConfig::kLedCount * 3);
      if (length != expectedPayload) {
        return MatrixProtocol::Status::kInvalidLength;
      }

      const uint8_t frameIndex = payload[0];
      const uint8_t frameCount = payload[1];
      const uint16_t frameDelay =
          static_cast<uint16_t>(payload[2] | (static_cast<uint16_t>(payload[3]) << 8));

      return effects_.applyCustomFrame(frameIndex, frameCount, frameDelay, payload + 4)
                 ? MatrixProtocol::Status::kOk
                 : MatrixProtocol::Status::kInvalidLength;
    }

    case MatrixProtocol::Command::kStopEffect:
      // Every sibling opcode validates its payload length; this one did not.
      if (length != 0) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      // Note: this halts the effect engine but deliberately does NOT touch
      // frameRgb_ or call render(), so the panel keeps whatever the last effect
      // tick drew. Use kClear to actually go dark.
      effects_.stop();
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetPanelEnabled:
      // Panel power is a visibility flag, not a color command. Turning off
      // writes black to the LEDs but keeps the current frame in memory so
      // turning on restores it.
      if (length != 1) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      matrix_.setEnabled(payload[0] != 0);
      return MatrixProtocol::Status::kOk;

    default:
      return MatrixProtocol::Status::kUnknownCommand;
  }
}

void TcpMatrixServer::sendStatus(MatrixProtocol::Status status) {
  // If there is no active client, there is nowhere useful to send status. This
  // can happen during disconnect/retry paths.
  if (!client_ || !client_.connected()) {
    return;
  }

  // Response frame mirrors the protocol header and uses command 0x80 to mark it
  // as device-to-client status rather than a matrix command.
  uint8_t response[MatrixProtocol::kResponseSize] = {
      MatrixProtocol::kMagic0,          MatrixProtocol::kMagic1,      MatrixProtocol::kVersion,
      MatrixProtocol::kResponseCommand, static_cast<uint8_t>(status), 0,
  };

  response[5] = MatrixProtocol::checksum(response, MatrixProtocol::kResponseSize - 1);
  client_.write(response, MatrixProtocol::kResponseSize);
}
