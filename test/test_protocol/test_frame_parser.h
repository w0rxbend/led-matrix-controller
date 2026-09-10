// Host-side tests for the streaming frame parser.
//
// This is the untrusted boundary: every byte here arrives from an unauthenticated
// TCP socket, and all the framing and length checks live in FrameParser. Before
// it was extracted, none of this could be tested off the board because it was
// welded to WiFiClient.

#pragma once

#include <unity.h>

#include "FrameParser.h"

namespace {

// Feeds a byte sequence and returns the result of the last byte.
FrameParser::Result feedAll(FrameParser& parser, const uint8_t* bytes, size_t length) {
  FrameParser::Result result;
  for (size_t i = 0; i < length; i++) {
    result = parser.feed(bytes[i]);
  }
  return result;
}

// Counts how many replies a sequence would put on the wire.
int countStatusReplies(FrameParser& parser, const uint8_t* bytes, size_t length) {
  int replies = 0;
  for (size_t i = 0; i < length; i++) {
    if (parser.feed(bytes[i]).action == FrameParser::Action::kSendStatus) {
      replies++;
    }
  }
  return replies;
}

}  // namespace

// ── Well-formed frames ──────────────────────────────────────────────────────

void test_parser_accepts_the_documented_ping_frame() {
  // CLIENT_PROTOCOL.md: "Ping: 4C 4D 01 00 00 00"
  const uint8_t frame[] = {0x4C, 0x4D, 0x01, 0x00, 0x00, 0x00};
  FrameParser parser;
  const FrameParser::Result result = feedAll(parser, frame, sizeof(frame));

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kFrameReady);
  TEST_ASSERT_EQUAL_UINT8(0x00, parser.command());
  TEST_ASSERT_EQUAL_UINT8(0, parser.payloadLength());
}

void test_parser_exposes_the_payload_of_a_fill_frame() {
  // "Fill red: 4C 4D 01 03 03 FF 00 00 FF"
  const uint8_t frame[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF, 0x00, 0x00, 0xFF};
  FrameParser parser;
  const FrameParser::Result result = feedAll(parser, frame, sizeof(frame));

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kFrameReady);
  TEST_ASSERT_EQUAL_UINT8(0x03, parser.command());
  TEST_ASSERT_EQUAL_UINT8(3, parser.payloadLength());
  TEST_ASSERT_EQUAL_UINT8(0xFF, parser.payload()[0]);
  TEST_ASSERT_EQUAL_UINT8(0x00, parser.payload()[1]);
  TEST_ASSERT_EQUAL_UINT8(0x00, parser.payload()[2]);
}

void test_parser_accepts_two_frames_back_to_back() {
  const uint8_t two[] = {0x4C, 0x4D, 0x01, 0x00, 0x00, 0x00,
                         0x4C, 0x4D, 0x01, 0x01, 0x00, 0x01};
  FrameParser parser;
  int ready = 0;
  for (size_t i = 0; i < sizeof(two); i++) {
    if (parser.feed(two[i]).action == FrameParser::Action::kFrameReady) {
      ready++;
    }
  }
  TEST_ASSERT_EQUAL_INT(2, ready);
}

void test_parser_holds_state_across_a_split_frame() {
  // The transport may deliver a frame one byte at a time across loop() passes.
  const uint8_t frame[] = {0x4C, 0x4D, 0x01, 0x02, 0x01, 0x14, 0x17};
  FrameParser parser;

  for (size_t i = 0; i + 1 < sizeof(frame); i++) {
    TEST_ASSERT_TRUE(parser.feed(frame[i]).action == FrameParser::Action::kNeedMoreBytes);
  }
  TEST_ASSERT_TRUE(parser.feed(frame[sizeof(frame) - 1]).action ==
                   FrameParser::Action::kFrameReady);
  TEST_ASSERT_EQUAL_UINT8(0x14, parser.payload()[0]);
}

void test_parser_accepts_the_largest_frame_a_length_byte_can_describe() {
  // 255 payload bytes is the ceiling, and the buffer must hold it.
  uint8_t frame[MatrixProtocol::kMaxFrameSize] = {0x4C, 0x4D, 0x01, 0x05, 0xFF};
  for (uint16_t i = 0; i < 255; i++) {
    frame[MatrixProtocol::kHeaderSize + i] = static_cast<uint8_t>(i);
  }
  frame[MatrixProtocol::kMaxFrameSize - 1] =
      MatrixProtocol::checksum(frame, MatrixProtocol::kMaxFrameSize - 1);

  FrameParser parser;
  const FrameParser::Result result = feedAll(parser, frame, sizeof(frame));
  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kFrameReady);
  TEST_ASSERT_EQUAL_UINT8(255, parser.payloadLength());
}

// ── Malformed input ─────────────────────────────────────────────────────────

void test_parser_rejects_bad_magic() {
  FrameParser parser;
  const FrameParser::Result result = parser.feed(0x00);

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kSendStatus);
  TEST_ASSERT_TRUE(result.status == MatrixProtocol::Status::kBadMagic);
}

void test_parser_rejects_a_wrong_second_magic_byte() {
  FrameParser parser;
  TEST_ASSERT_TRUE(parser.feed(0x4C).action == FrameParser::Action::kNeedMoreBytes);
  const FrameParser::Result result = parser.feed(0x00);

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kSendStatus);
  TEST_ASSERT_TRUE(result.status == MatrixProtocol::Status::kBadMagic);
}

void test_parser_rejects_an_unsupported_version() {
  const uint8_t bytes[] = {0x4C, 0x4D, 0x99};
  FrameParser parser;
  const FrameParser::Result result = feedAll(parser, bytes, sizeof(bytes));

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kSendStatus);
  TEST_ASSERT_TRUE(result.status == MatrixProtocol::Status::kUnsupportedVersion);
}

void test_parser_rejects_a_bad_checksum() {
  const uint8_t frame[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF, 0x00, 0x00, 0x00};
  FrameParser parser;
  const FrameParser::Result result = feedAll(parser, frame, sizeof(frame));

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kSendStatus);
  TEST_ASSERT_TRUE(result.status == MatrixProtocol::Status::kChecksumMismatch);
}

void test_parser_never_reports_a_truncated_frame_as_ready() {
  // A client that sends a header promising 3 payload bytes and then stops must
  // leave the parser waiting, not dispatch a half-built command.
  const uint8_t truncated[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF};
  FrameParser parser;
  const FrameParser::Result result = feedAll(parser, truncated, sizeof(truncated));
  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kNeedMoreBytes);
}

// ── Response amplification ──────────────────────────────────────────────────

void test_stray_bytes_draw_exactly_one_reply() {
  // Every stray byte used to draw its own six-byte status frame, turning any
  // non-protocol traffic into a six-fold amplifier and keeping the read loop
  // busy writing. One reply per desync is all a client can act on.
  const char probe[] = "GET / HTTP/1.1\r\nHost: matrix\r\n\r\n";
  FrameParser parser;
  const int replies =
      countStatusReplies(parser, reinterpret_cast<const uint8_t*>(probe), sizeof(probe) - 1);

  TEST_ASSERT_EQUAL_INT(1, replies);
  TEST_ASSERT_TRUE(parser.resyncing());
}

void test_a_long_garbage_stream_still_draws_one_reply() {
  FrameParser parser;
  int replies = 0;
  for (int i = 0; i < 4096; i++) {
    // 0x00 is never the first magic byte.
    if (parser.feed(0x00).action == FrameParser::Action::kSendStatus) {
      replies++;
    }
  }
  TEST_ASSERT_EQUAL_INT(1, replies);
}

void test_the_parser_recovers_on_the_next_valid_frame() {
  // After garbage, a well-formed frame must still be accepted -- staying quiet
  // must not mean staying deaf.
  FrameParser parser;
  for (int i = 0; i < 32; i++) {
    parser.feed(0xAB);
  }
  TEST_ASSERT_TRUE(parser.resyncing());

  const uint8_t ping[] = {0x4C, 0x4D, 0x01, 0x00, 0x00, 0x00};
  const FrameParser::Result result = feedAll(parser, ping, sizeof(ping));

  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kFrameReady);
  TEST_ASSERT_FALSE(parser.resyncing());
  TEST_ASSERT_EQUAL_UINT8(0x00, parser.command());
}

void test_a_failed_frame_does_not_poison_the_next_one() {
  const uint8_t bad[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF, 0x00, 0x00, 0x00};
  const uint8_t good[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF, 0x00, 0x00, 0xFF};

  FrameParser parser;
  TEST_ASSERT_TRUE(feedAll(parser, bad, sizeof(bad)).action ==
                   FrameParser::Action::kSendStatus);
  TEST_ASSERT_TRUE(feedAll(parser, good, sizeof(good)).action ==
                   FrameParser::Action::kFrameReady);
  TEST_ASSERT_EQUAL_UINT8(0xFF, parser.payload()[0]);
}

void test_reset_discards_a_partial_frame() {
  FrameParser parser;
  parser.feed(0x4C);
  parser.feed(0x4D);
  parser.reset();

  // If the partial header survived, this byte would be read as the version.
  const FrameParser::Result result = parser.feed(0x01);
  TEST_ASSERT_TRUE(result.action == FrameParser::Action::kSendStatus);
  TEST_ASSERT_TRUE(result.status == MatrixProtocol::Status::kBadMagic);
}
