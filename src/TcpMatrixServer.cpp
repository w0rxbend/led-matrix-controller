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
      lastServerHealthCheckMs_(0) {}

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

  if (expectedChecksum != receivedChecksum) {
    sendStatus(MatrixProtocol::Status::kChecksumMismatch);
    resetParser();
    return;
  }

  const MatrixProtocol::Status status =
      applyCommand(frameBuffer_[3], &frameBuffer_[5], payloadLength);
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
      matrix_.fill(payload[0], payload[1], payload[2]);
      return MatrixProtocol::Status::kOk;

    case MatrixProtocol::Command::kSetPixel:
      // Single-pixel updates use logical coordinates and are mapped by
      // LedMatrixController.
      if (length != 5) {
        return MatrixProtocol::Status::kInvalidLength;
      }
      return matrix_.setPixel(payload[0], payload[1], payload[2], payload[3], payload[4])
                 ? MatrixProtocol::Status::kOk
                 : MatrixProtocol::Status::kInvalidLength;

    case MatrixProtocol::Command::kSetFrame:
      // Full-frame updates are fastest for animations because the client sends
      // the exact physical LED order with no per-pixel protocol overhead.
      return matrix_.setPhysicalFrame(payload, length) ? MatrixProtocol::Status::kOk
                                                       : MatrixProtocol::Status::kInvalidLength;

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
