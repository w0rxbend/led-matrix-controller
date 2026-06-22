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

uint8_t kColorWaveSteps[8] = {0, 2, 4, 6, 7, 5, 3, 1};
uint8_t kBreathingSteps[16] = {10, 18, 28, 42, 60, 84, 112, 150,
                               190, 150, 112, 84, 60, 42, 28, 18};
uint8_t kHeartbeatSteps[12] = {0, 180, 255, 70, 0, 0, 120, 210, 55, 0, 0, 0};

uint16_t clampDelayMs(uint16_t delayMs) {
  if (delayMs == 0) {
    return AppConfig::kDefaultPresetIntervalMs;
  }
  return delayMs < AppConfig::kMinEffectFrameDelayMs ? AppConfig::kMinEffectFrameDelayMs : delayMs;
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

void logInstruction(uint8_t command, uint8_t payloadLength, const IPAddress& remoteIp, uint16_t remotePort) {
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
  constexpr uint8_t perimeterLength = (AppConfig::kMatrixWidth * 2) + (AppConfig::kMatrixHeight * 2) - 4;
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

TcpMatrixServer::TcpMatrixServer(LedMatrixController& matrix)
    : matrix_(matrix),
      server_(AppConfig::kTcpPort),
      // Zeroing the frame buffer is not required for correctness, but it makes
      // startup state explicit and keeps static analysis happy.
      frameBuffer_(),
      frameIndex_(0),
      expectedFrameSize_(0),
      serverStarted_(false),
      lastWifiRetryMs_(0),
      lastServerHealthCheckMs_(0),
      effectMode_(EffectMode::kDirect),
      effectIntervalMs_(AppConfig::kDefaultPresetIntervalMs),
      lastEffectStepMs_(0),
      effectPhase_(0),
      effectColorRed_(255),
      effectColorGreen_(255),
      effectColorBlue_(255),
      effectBlinkState_(false),
      effectSeed_(0xDEADBEEF),
      customFrameCount_(0),
      customFrameExpectedCount_(0),
      customReceivedMask_(0),
      customCurrentFrame_(0) {
  memset(customFrameDelayMs_, 0, sizeof(customFrameDelayMs_));
  memset(customFrameBuffer_, 0, sizeof(customFrameBuffer_));
}

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
  // 3. Accept/read client data only when the listener is valid.
  handleWifiReconnect();
  ensureServerRunning();
  acceptClientIfNeeded();
  readClientBytes();
  updateAnimations();
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
  WiFi.softAP(AppConfig::kAccessPointSsid);

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
  resetParser();
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
  resetParser();
  Serial.println("TCP client connected");
}

void TcpMatrixServer::readClientBytes() {
  // TCP may disconnect between loop iterations. Treat that as normal.
  if (!client_ || !client_.connected()) {
    return;
  }

  // Consume every currently buffered byte. parseByte() keeps frame state across
  // calls, so partial frames are fine.
  while (client_.available() > 0) {
    parseByte(static_cast<uint8_t>(client_.read()));
  }
}

void TcpMatrixServer::resetParser() {
  // The buffer contents do not need clearing; frameIndex_ defines which bytes
  // are valid. Resetting only counters is faster and simpler.
  frameIndex_ = 0;
  expectedFrameSize_ = 0;
}

void TcpMatrixServer::parseByte(uint8_t value) {
  // Validate the fixed header as early as possible. That lets us recover quickly
  // from clients connecting mid-stream or sending text by mistake.
  if (frameIndex_ == 0 && value != MatrixProtocol::kMagic0) {
    sendStatus(MatrixProtocol::Status::kBadMagic);
    return;
  }

  if (frameIndex_ == 1 && value != MatrixProtocol::kMagic1) {
    sendStatus(MatrixProtocol::Status::kBadMagic);
    resetParser();
    return;
  }

  if (frameIndex_ == 2 && value != MatrixProtocol::kVersion) {
    sendStatus(MatrixProtocol::Status::kUnsupportedVersion);
    resetParser();
    return;
  }

  frameBuffer_[frameIndex_] = value;
  frameIndex_++;

  // Once the 5-byte header has arrived we know the total frame size. The length
  // byte is capped so the parser can never write past frameBuffer_.
  if (frameIndex_ == MatrixProtocol::kHeaderSize) {
    const uint8_t payloadLength = frameBuffer_[4];
    if (payloadLength > MatrixProtocol::kMaxPayloadSize) {
      sendStatus(MatrixProtocol::Status::kInvalidLength);
      resetParser();
      return;
    }

    expectedFrameSize_ =
        MatrixProtocol::kHeaderSize + payloadLength + MatrixProtocol::kChecksumSize;
  }

  // A complete frame is parsed only after header + payload + checksum arrive.
  if (expectedFrameSize_ > 0 && frameIndex_ == expectedFrameSize_) {
    processFrame();
  }
}

void TcpMatrixServer::processFrame() {
  // Header validation already happened in parseByte(). Here we only verify the
  // checksum and then dispatch the command.
  const uint8_t payloadLength = frameBuffer_[4];
  const uint8_t receivedChecksum = frameBuffer_[expectedFrameSize_ - 1];
  const uint8_t expectedChecksum = MatrixProtocol::checksum(frameBuffer_, expectedFrameSize_ - 1);
  const uint8_t command = frameBuffer_[3];

  if (client_) {
    logInstruction(command, payloadLength, client_.remoteIP(), client_.remotePort());
  }

  if (expectedChecksum != receivedChecksum) {
    sendStatus(MatrixProtocol::Status::kChecksumMismatch);
    resetParser();
    return;
  }

  const MatrixProtocol::Status status = applyCommand(command, &frameBuffer_[5], payloadLength);
  sendStatus(status);
  resetParser();
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
      stopEffects();
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
      stopEffects();
      matrix_.fill(payload[0], payload[1], payload[2]);
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetPixel:
      // Single-pixel updates use logical coordinates and are mapped by
      // LedMatrixController.
      if (length != 5) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      stopEffects();
      return matrix_.setPixel(payload[0], payload[1], payload[2], payload[3], payload[4])
                 ? MatrixProtocol::Status::kOk
                 : MatrixProtocol::Status::kInvalidLength;

    case MatrixProtocol::Command::kSetFrame:
      // Full-frame updates are fastest for animations because the client sends
      // the exact physical LED order with no per-pixel protocol overhead.
      if (length != AppConfig::kLedCount * 3) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      stopEffects();
      return matrix_.setPhysicalFrame(payload, length) ? MatrixProtocol::Status::kOk
                                                      : MatrixProtocol::Status::kInvalidLength;

    case MatrixProtocol::Command::kSetStaticColor:
      // Direct control of static color. The firmware keeps showing the color.
      if (length != 3) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      startEffect(EffectMode::kStatic, AppConfig::kDefaultPresetIntervalMs, payload[0], payload[1],
                  payload[2]);
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetPresetEffect: {
      // Payload: effectId, interval LSB, interval MSB, red, green, blue.
      if (length != 6) {
        return MatrixProtocol::Status::kInvalidLength;
      }

      const uint8_t effectId = payload[0];
      if (effectId == 0) {
        stopEffects();
        return MatrixProtocol::Status::kOk;
      }

      const uint16_t intervalMs = clampDelayMs(static_cast<uint16_t>(payload[1] | (payload[2] << 8)));
      const uint8_t r = payload[3];
      const uint8_t g = payload[4];
      const uint8_t b = payload[5];

      switch (effectId) {
        case 1:
          startEffect(EffectMode::kChase, intervalMs, r, g, b);
          break;
        case 2:
          startEffect(EffectMode::kColorWipe, intervalMs, r, g, b);
          break;
        case 3:
          startEffect(EffectMode::kBlink, intervalMs, r, g, b);
          break;
        case 4:
          startEffect(EffectMode::kWave, intervalMs, r, g, b);
          break;
        case 5:
          startEffect(EffectMode::kRain, intervalMs, r, g, b);
          break;
        case 6:
          startEffect(EffectMode::kMeteor, intervalMs, r, g, b);
          break;
        case 7:
          startEffect(EffectMode::kRainbow, intervalMs, r, g, b);
          break;
        case 8:
          startEffect(EffectMode::kBreathing, intervalMs, r, g, b);
          break;
        case 9:
          startEffect(EffectMode::kScanner, intervalMs, r, g, b);
          break;
        case 10:
          startEffect(EffectMode::kSparkle, intervalMs, r, g, b);
          break;
        case 11:
          startEffect(EffectMode::kFire, intervalMs, r, g, b);
          break;
        case 12:
          startEffect(EffectMode::kMatrixRain, intervalMs, r, g, b);
          break;
        case 13:
          startEffect(EffectMode::kRipple, intervalMs, r, g, b);
          break;
        case 14:
          startEffect(EffectMode::kTheaterChase, intervalMs, r, g, b);
          break;
        case 15:
          startEffect(EffectMode::kTwinkle, intervalMs, r, g, b);
          break;
        case 16:
          startEffect(EffectMode::kComet, intervalMs, r, g, b);
          break;
        case 17:
          startEffect(EffectMode::kPlasma, intervalMs, r, g, b);
          break;
        case 18:
          startEffect(EffectMode::kDiagonal, intervalMs, r, g, b);
          break;
        case 19:
          startEffect(EffectMode::kBorderChase, intervalMs, r, g, b);
          break;
        case 20:
          startEffect(EffectMode::kHeartbeat, intervalMs, r, g, b);
          break;
        case 21:
          startEffect(EffectMode::kPulseWipe, intervalMs, r, g, b);
          break;
        case 22:
          startEffect(EffectMode::kConfetti, intervalMs, r, g, b);
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

      return applyCustomFrame(frameIndex, frameCount, frameDelay, payload + 4)
                 ? MatrixProtocol::Status::kOk
                 : MatrixProtocol::Status::kInvalidLength;
    }

    case MatrixProtocol::Command::kStopEffect:
      stopEffects();
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

void TcpMatrixServer::stopEffects() {
  effectMode_ = EffectMode::kDirect;
  effectPhase_ = 0;
  effectBlinkState_ = false;
  customFrameCount_ = 0;
  customFrameExpectedCount_ = 0;
  customReceivedMask_ = 0;
}

void TcpMatrixServer::startEffect(EffectMode mode, uint16_t intervalMs, uint8_t red, uint8_t green,
                                 uint8_t blue) {
  effectMode_ = mode;
  effectIntervalMs_ = intervalMs;
  effectColorRed_ = red;
  effectColorGreen_ = green;
  effectColorBlue_ = blue;
  effectPhase_ = 0;
  effectBlinkState_ = false;
  lastEffectStepMs_ = 0;

  renderEffectFrame(millis());
}

bool TcpMatrixServer::applyCustomFrame(uint8_t frameIndex, uint8_t frameCount, uint16_t delayMs,
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
    startEffect(EffectMode::kCustom, AppConfig::kDefaultPresetIntervalMs, 255, 255, 255);
    renderCustom(millis());
  }
  return true;
}

void TcpMatrixServer::updateAnimations() {
  if (effectMode_ == EffectMode::kDirect) {
    return;
  }

  const uint32_t nowMs = millis();
  renderEffectFrame(nowMs);
}

void TcpMatrixServer::renderEffectFrame(uint32_t nowMs) {
  if (effectMode_ == EffectMode::kCustom) {
    renderCustom(nowMs);
    return;
  }

  if ((effectIntervalMs_ == 0) || (nowMs - lastEffectStepMs_ < effectIntervalMs_)) {
    return;
  }

  lastEffectStepMs_ = nowMs;
  effectPhase_++;

  switch (effectMode_) {
    case EffectMode::kStatic:
      renderStatic(nowMs);
      break;
    case EffectMode::kChase:
      renderChase(nowMs);
      break;
    case EffectMode::kColorWipe:
      renderColorWipe(nowMs);
      break;
    case EffectMode::kBlink:
      renderBlink(nowMs);
      break;
    case EffectMode::kWave:
      renderWave(nowMs);
      break;
    case EffectMode::kRain:
      renderRain(nowMs);
      break;
    case EffectMode::kMeteor:
      renderMeteor(nowMs);
      break;
    case EffectMode::kRainbow:
      renderRainbow(nowMs);
      break;
    case EffectMode::kBreathing:
      renderBreathing(nowMs);
      break;
    case EffectMode::kScanner:
      renderScanner(nowMs);
      break;
    case EffectMode::kSparkle:
      renderSparkle(nowMs);
      break;
    case EffectMode::kFire:
      renderFire(nowMs);
      break;
    case EffectMode::kMatrixRain:
      renderMatrixRain(nowMs);
      break;
    case EffectMode::kRipple:
      renderRipple(nowMs);
      break;
    case EffectMode::kTheaterChase:
      renderTheaterChase(nowMs);
      break;
    case EffectMode::kTwinkle:
      renderTwinkle(nowMs);
      break;
    case EffectMode::kComet:
      renderComet(nowMs);
      break;
    case EffectMode::kPlasma:
      renderPlasma(nowMs);
      break;
    case EffectMode::kDiagonal:
      renderDiagonal(nowMs);
      break;
    case EffectMode::kBorderChase:
      renderBorderChase(nowMs);
      break;
    case EffectMode::kHeartbeat:
      renderHeartbeat(nowMs);
      break;
    case EffectMode::kPulseWipe:
      renderPulseWipe(nowMs);
      break;
    case EffectMode::kConfetti:
      renderConfetti(nowMs);
      break;
    default:
      break;
  }
}

void TcpMatrixServer::renderStatic(uint32_t /*nowMs*/) {
  matrix_.fill(effectColorRed_, effectColorGreen_, effectColorBlue_);
}

void TcpMatrixServer::renderChase(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint16_t ledIndex = static_cast<uint16_t>(effectPhase_ % AppConfig::kLedCount);
  const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
  const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
  const uint8_t physicalIndex = logicalToPhysical(x, y);

  if (physicalIndex < AppConfig::kLedCount) {
    const uint16_t base = physicalIndex * 3;
    frame[base] = effectColorRed_;
    frame[base + 1] = effectColorGreen_;
    frame[base + 2] = effectColorBlue_;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderColorWipe(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t targetCount = static_cast<uint8_t>(effectPhase_ % (AppConfig::kLedCount + 1));

  for (uint8_t ledIndex = 0; ledIndex < targetCount; ++ledIndex) {
    const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
    const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
    const uint8_t physicalIndex = logicalToPhysical(x, y);
    const uint16_t base = static_cast<uint16_t>(physicalIndex) * 3;
    frame[base] = effectColorRed_;
    frame[base + 1] = effectColorGreen_;
    frame[base + 2] = effectColorBlue_;
  }

  if (targetCount == AppConfig::kLedCount) {
    effectPhase_ = 0;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderBlink(uint32_t /*nowMs*/) {
  effectBlinkState_ = !effectBlinkState_;
  if (effectBlinkState_) {
    matrix_.fill(effectColorRed_, effectColorGreen_, effectColorBlue_);
  } else {
    matrix_.clear();
  }
}

void TcpMatrixServer::renderWave(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t wave = kColorWaveSteps[(x + effectPhase_ + y) % 8];
      const uint16_t base = static_cast<uint16_t>(logicalToPhysical(x, y)) * 3;
      frame[base] = static_cast<uint8_t>((static_cast<uint16_t>(effectColorRed_) * wave) / 7);
      frame[base + 1] =
          static_cast<uint8_t>((static_cast<uint16_t>(effectColorGreen_) * wave) / 7);
      frame[base + 2] =
          static_cast<uint8_t>((static_cast<uint16_t>(effectColorBlue_) * wave) / 7);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderRain(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
    const uint8_t spawn = static_cast<uint8_t>(xorshift32(effectSeed_) % 100);
    if (spawn < 20) {
      const uint8_t dropY = static_cast<uint8_t>(xorshift32(effectSeed_) % AppConfig::kMatrixHeight);
      const uint8_t base = static_cast<uint16_t>(logicalToPhysical(x, dropY)) * 3;
      frame[base] = effectColorRed_;
      frame[base + 1] = effectColorGreen_;
      frame[base + 2] = effectColorBlue_;
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderMeteor(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t tailLength = 4;

  for (uint8_t tail = 0; tail < tailLength; ++tail) {
    const int16_t step = static_cast<int16_t>(effectPhase_ - tail);
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
    frame[base] =
        static_cast<uint8_t>((static_cast<uint16_t>(effectColorRed_) * scale) / 192);
    frame[base + 1] =
        static_cast<uint8_t>((static_cast<uint16_t>(effectColorGreen_) * scale) / 192);
    frame[base + 2] =
        static_cast<uint8_t>((static_cast<uint16_t>(effectColorBlue_) * scale) / 192);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderRainbow(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      uint8_t red = 0;
      uint8_t green = 0;
      uint8_t blue = 0;
      wheelColor(static_cast<uint8_t>(effectPhase_ * 5 + x * 18 + y * 9), red, green, blue);
      setFramePixel(frame, x, y, red, green, blue);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderBreathing(uint32_t /*nowMs*/) {
  const uint8_t scale = kBreathingSteps[effectPhase_ % 16];
  matrix_.fill(scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
               scaled(effectColorBlue_, scale));
}

void TcpMatrixServer::renderScanner(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t span = (AppConfig::kMatrixWidth * 2) - 2;
  const uint8_t position = effectPhase_ % span;
  const uint8_t scannerX = position < AppConfig::kMatrixWidth ? position : span - position;

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t distance = x > scannerX ? x - scannerX : scannerX - x;
      if (distance > 3) {
        continue;
      }

      const uint8_t scale = distance == 0 ? 255 : distance == 1 ? 120 : distance == 2 ? 45 : 12;
      setFramePixel(frame, x, y, scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
                    scaled(effectColorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderSparkle(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t spark = 0; spark < 7; ++spark) {
    const uint8_t ledIndex = static_cast<uint8_t>(xorshift32(effectSeed_) % AppConfig::kLedCount);
    const uint8_t scale = spark < 2 ? 255 : 80;
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = scaled(effectColorRed_, scale);
    frame[base + 1] = scaled(effectColorGreen_, scale);
    frame[base + 2] = scaled(effectColorBlue_, scale);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderFire(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    const uint8_t heightHeat = static_cast<uint8_t>(255 - (y * 28));
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t noise = static_cast<uint8_t>(xorshift32(effectSeed_) % 85);
      const uint8_t heat = heightHeat > noise ? heightHeat - noise : 0;
      const uint8_t red = heat;
      const uint8_t green = heat > 128 ? static_cast<uint8_t>((heat - 128) * 2) : heat / 3;
      const uint8_t blue = heat > 230 ? static_cast<uint8_t>((heat - 230) * 4) : 0;
      setFramePixel(frame, x, AppConfig::kMatrixHeight - 1 - y, red, green, blue);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderMatrixRain(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
    const uint8_t head = static_cast<uint8_t>((effectPhase_ + x * 3) % (AppConfig::kMatrixHeight + 4));
    for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
      if (head < y || head - y > 3) {
        continue;
      }

      const uint8_t distance = head - y;
      const uint8_t scale = distance == 0 ? 255 : distance == 1 ? 120 : distance == 2 ? 50 : 16;
      setFramePixel(frame, x, y, scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
                    scaled(effectColorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderRipple(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t radius = effectPhase_ % 8;

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
      setFramePixel(frame, x, y, scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
                    scaled(effectColorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderTheaterChase(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t phase = effectPhase_ % 3;

  for (uint8_t ledIndex = 0; ledIndex < AppConfig::kLedCount; ++ledIndex) {
    if ((ledIndex + phase) % 3 != 0) {
      continue;
    }

    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = effectColorRed_;
    frame[base + 1] = effectColorGreen_;
    frame[base + 2] = effectColorBlue_;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderTwinkle(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t sparkle = 0; sparkle < 10; ++sparkle) {
    const uint8_t ledIndex = static_cast<uint8_t>(xorshift32(effectSeed_) % AppConfig::kLedCount);
    const uint8_t scale = static_cast<uint8_t>(30 + (xorshift32(effectSeed_) % 226));
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = scaled(effectColorRed_, scale);
    frame[base + 1] = scaled(effectColorGreen_, scale);
    frame[base + 2] = scaled(effectColorBlue_, scale);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderComet(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  constexpr uint8_t tailLength = 8;
  const uint8_t head = effectPhase_ % AppConfig::kLedCount;

  for (uint8_t tail = 0; tail < tailLength; ++tail) {
    const uint8_t ledIndex = static_cast<uint8_t>((head + AppConfig::kLedCount - tail) % AppConfig::kLedCount);
    const uint8_t scale = static_cast<uint8_t>(255 - (tail * 28));
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = scaled(effectColorRed_, scale);
    frame[base + 1] = scaled(effectColorGreen_, scale);
    frame[base + 2] = scaled(effectColorBlue_, scale);
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderPlasma(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      uint8_t red = 0;
      uint8_t green = 0;
      uint8_t blue = 0;
      const uint8_t position =
          static_cast<uint8_t>((x * x * 9) + (y * y * 7) + (x * y * 5) + (effectPhase_ * 6));
      wheelColor(position, red, green, blue);
      setFramePixel(frame, x, y, red, green, blue);
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderDiagonal(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; ++x) {
      const uint8_t band = (x + y + effectPhase_) % 6;
      if (band > 2) {
        continue;
      }

      const uint8_t scale = band == 0 ? 255 : band == 1 ? 110 : 35;
      setFramePixel(frame, x, y, scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
                    scaled(effectColorBlue_, scale));
    }
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderBorderChase(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  constexpr uint8_t perimeterLength = (AppConfig::kMatrixWidth * 2) + (AppConfig::kMatrixHeight * 2) - 4;

  for (uint8_t tail = 0; tail < 6; ++tail) {
    uint8_t x = 0;
    uint8_t y = 0;
    perimeterToPoint(static_cast<uint8_t>(effectPhase_ + perimeterLength - tail), x, y);
    const uint8_t scale = static_cast<uint8_t>(255 - (tail * 35));
    setFramePixel(frame, x, y, scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
                  scaled(effectColorBlue_, scale));
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderHeartbeat(uint32_t /*nowMs*/) {
  const uint8_t scale = kHeartbeatSteps[effectPhase_ % 12];
  matrix_.fill(scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
               scaled(effectColorBlue_, scale));
}

void TcpMatrixServer::renderPulseWipe(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};
  const uint8_t scale = kBreathingSteps[effectPhase_ % 16];
  const uint8_t targetCount = static_cast<uint8_t>(effectPhase_ % (AppConfig::kLedCount + 1));

  for (uint8_t ledIndex = 0; ledIndex < targetCount; ++ledIndex) {
    const uint8_t x = static_cast<uint8_t>(ledIndex % AppConfig::kMatrixWidth);
    const uint8_t y = static_cast<uint8_t>(ledIndex / AppConfig::kMatrixWidth);
    setFramePixel(frame, x, y, scaled(effectColorRed_, scale), scaled(effectColorGreen_, scale),
                  scaled(effectColorBlue_, scale));
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderConfetti(uint32_t /*nowMs*/) {
  uint8_t frame[AppConfig::kLedCount * 3] = {};

  for (uint8_t dot = 0; dot < 9; ++dot) {
    const uint8_t ledIndex = static_cast<uint8_t>(xorshift32(effectSeed_) % AppConfig::kLedCount);
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    wheelColor(static_cast<uint8_t>(effectPhase_ * 11 + ledIndex * 17 + dot * 23), red, green, blue);
    const uint16_t base = static_cast<uint16_t>(ledIndex) * 3;
    frame[base] = red;
    frame[base + 1] = green;
    frame[base + 2] = blue;
  }

  matrix_.setPhysicalFrame(frame, sizeof(frame));
}

void TcpMatrixServer::renderCustom(uint32_t nowMs) {
  if (customFrameExpectedCount_ == 0 || customFrameCount_ < customFrameExpectedCount_) {
    return;
  }

  const uint16_t delayMs = customFrameDelayMs_[customCurrentFrame_] == 0
                              ? AppConfig::kDefaultPresetIntervalMs
                              : customFrameDelayMs_[customCurrentFrame_];
  if (nowMs - lastEffectStepMs_ < delayMs) {
    return;
  }

  lastEffectStepMs_ = nowMs;
  matrix_.setPhysicalFrame(customFrameBuffer_[customCurrentFrame_],
                          AppConfig::kLedCount * 3);
  customCurrentFrame_ = (customCurrentFrame_ + 1) % customFrameExpectedCount_;
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
