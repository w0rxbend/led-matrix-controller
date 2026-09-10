#pragma once

#include <stdint.h>

#include "MatrixProtocol.h"

// Streaming parser for one protocol frame.
//
// This is deliberately free of Arduino, WiFiClient and LED dependencies. It is
// the boundary every hostile byte crosses -- all the framing and length checks
// live here -- so it has to be reachable by the host test build rather than only
// on the board. TcpMatrixServer feeds it bytes and acts on what it returns.
class FrameParser {
 public:
  // What the caller should do with the byte just fed in.
  enum class Action : uint8_t {
    // Nothing to do; the frame is still arriving, or the byte was discarded.
    kNeedMoreBytes,

    // A framing error the client has not been told about yet. Reply with
    // `status`, then keep feeding: the parser stays quiet about the bytes that
    // follow until the stream resynchronises.
    kSendStatus,

    // A complete frame passed its checksum. command(), payload() and
    // payloadLength() describe it until the next feed() call.
    kFrameReady,
  };

  struct Result {
    Action action = Action::kNeedMoreBytes;
    MatrixProtocol::Status status = MatrixProtocol::Status::kOk;
  };

  // Drops any partial frame and clears the resync state. Call on a new client.
  void reset();

  Result feed(uint8_t value);

  // Valid only while the last feed() returned kFrameReady.
  uint8_t command() const {
    return frameBuffer_[3];
  }
  const uint8_t* payload() const {
    return &frameBuffer_[MatrixProtocol::kHeaderSize];
  }
  uint8_t payloadLength() const {
    return frameBuffer_[4];
  }

  // Exposed for tests: true while stray bytes are being discarded silently.
  bool resyncing() const {
    return resyncing_;
  }

 private:
  Result failFraming(MatrixProtocol::Status status);
  void resetFrame();

  uint8_t frameBuffer_[MatrixProtocol::kMaxFrameSize] = {};
  uint16_t frameIndex_ = 0;
  uint16_t expectedFrameSize_ = 0;

  // True while discarding bytes after a framing error. Without this, every
  // stray byte drew its own six-byte reply, so an HTTP probe or a port scan
  // turned the panel into a six-fold response amplifier and kept the read loop
  // busy writing.
  bool resyncing_ = false;
};
