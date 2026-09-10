#include "FrameParser.h"

// The length byte is a uint8_t, so the largest frame it can describe is
// header + 255 + checksum. Asserting that at compile time is what actually
// keeps feed() from writing past frameBuffer_ -- the runtime check this
// replaced compared a uint8_t against 255 and could never be true.
static_assert(MatrixProtocol::kMaxFrameSize >=
                  MatrixProtocol::kHeaderSize + 255 + MatrixProtocol::kChecksumSize,
              "frameBuffer_ must hold the largest frame a uint8_t length byte can describe");

void FrameParser::reset() {
  resetFrame();
  resyncing_ = false;
}

void FrameParser::resetFrame() {
  // The buffer contents do not need clearing; frameIndex_ defines which bytes
  // are valid.
  frameIndex_ = 0;
  expectedFrameSize_ = 0;
}

FrameParser::Result FrameParser::failFraming(MatrixProtocol::Status status) {
  const bool alreadyReported = resyncing_;
  resetFrame();
  resyncing_ = true;

  // One reply per desync. A client can act on the first one; the rest only cost
  // bandwidth and loop() time.
  if (alreadyReported) {
    return Result{Action::kNeedMoreBytes, MatrixProtocol::Status::kOk};
  }
  return Result{Action::kSendStatus, status};
}

FrameParser::Result FrameParser::feed(uint8_t value) {
  // Validate the fixed header as early as possible. That lets us recover
  // quickly from clients connecting mid-stream or sending text by mistake.
  if (frameIndex_ == 0) {
    if (value != MatrixProtocol::kMagic0) {
      return failFraming(MatrixProtocol::Status::kBadMagic);
    }
    // A frame is starting, so the stream has resynchronised.
    resyncing_ = false;
  } else if (frameIndex_ == 1 && value != MatrixProtocol::kMagic1) {
    return failFraming(MatrixProtocol::Status::kBadMagic);
  } else if (frameIndex_ == 2 && value != MatrixProtocol::kVersion) {
    return failFraming(MatrixProtocol::Status::kUnsupportedVersion);
  }

  frameBuffer_[frameIndex_] = value;
  frameIndex_++;

  // Once the 5-byte header has arrived we know the total frame size.
  if (frameIndex_ == MatrixProtocol::kHeaderSize) {
    expectedFrameSize_ = static_cast<uint16_t>(MatrixProtocol::kHeaderSize + frameBuffer_[4] +
                                               MatrixProtocol::kChecksumSize);
  }

  if (expectedFrameSize_ == 0 || frameIndex_ != expectedFrameSize_) {
    return Result{Action::kNeedMoreBytes, MatrixProtocol::Status::kOk};
  }

  const uint8_t receivedChecksum = frameBuffer_[expectedFrameSize_ - 1];
  const uint8_t expectedChecksum =
      MatrixProtocol::checksum(frameBuffer_, static_cast<uint16_t>(expectedFrameSize_ - 1));
  if (receivedChecksum != expectedChecksum) {
    return failFraming(MatrixProtocol::Status::kChecksumMismatch);
  }

  // resetFrame() only moves the indices, so the buffer stays intact and the
  // caller can still read command(), payload() and payloadLength().
  resetFrame();
  return Result{Action::kFrameReady, MatrixProtocol::Status::kOk};
}
