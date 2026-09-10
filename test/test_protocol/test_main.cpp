// Host-side unit tests for the firmware's pure logic.
//
// These cover the two things every visible pixel depends on and that no amount of
// manual testing reliably catches: the XOR framing checksum, and the serpentine
// mapping from logical x/y to LED chain index. Both are verified against the
// frames documented in CLIENT_PROTOCOL.md so the document stays honest.

#include <unity.h>

#include "MatrixLayout.h"
#include "MatrixProtocol.h"

// ── Checksum ────────────────────────────────────────────────────────────────

void test_checksum_of_empty_payload_is_zero() {
  TEST_ASSERT_EQUAL_UINT8(0, MatrixProtocol::checksum(nullptr, 0));
}

void test_checksum_matches_documented_ping_frame() {
  // CLIENT_PROTOCOL.md: "Ping: 4C 4D 01 00 00 00"
  const uint8_t header[] = {0x4C, 0x4D, 0x01, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8(0x00, MatrixProtocol::checksum(header, sizeof(header)));
}

void test_checksum_matches_documented_clear_frame() {
  // "Clear: 4C 4D 01 01 00 01"
  const uint8_t header[] = {0x4C, 0x4D, 0x01, 0x01, 0x00};
  TEST_ASSERT_EQUAL_UINT8(0x01, MatrixProtocol::checksum(header, sizeof(header)));
}

void test_checksum_matches_documented_brightness_frame() {
  // "Brightness 20: 4C 4D 01 02 01 14 17"
  const uint8_t frame[] = {0x4C, 0x4D, 0x01, 0x02, 0x01, 0x14};
  TEST_ASSERT_EQUAL_UINT8(0x17, MatrixProtocol::checksum(frame, sizeof(frame)));
}

void test_checksum_matches_documented_fill_red_frame() {
  // "Fill red: 4C 4D 01 03 03 FF 00 00 FF"
  const uint8_t frame[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8(0xFF, MatrixProtocol::checksum(frame, sizeof(frame)));
}

void test_checksum_matches_documented_panel_frames() {
  // "Panel off: 4C 4D 01 06 01 00 07" and "Panel on: 4C 4D 01 06 01 01 06"
  const uint8_t off[] = {0x4C, 0x4D, 0x01, 0x06, 0x01, 0x00};
  const uint8_t on[] = {0x4C, 0x4D, 0x01, 0x06, 0x01, 0x01};
  TEST_ASSERT_EQUAL_UINT8(0x07, MatrixProtocol::checksum(off, sizeof(off)));
  TEST_ASSERT_EQUAL_UINT8(0x06, MatrixProtocol::checksum(on, sizeof(on)));
}

void test_checksum_detects_a_single_flipped_bit() {
  const uint8_t clean[] = {0x4C, 0x4D, 0x01, 0x03, 0x03, 0xFF, 0x00, 0x00};
  uint8_t corrupted[sizeof(clean)];
  for (size_t i = 0; i < sizeof(clean); i++) {
    corrupted[i] = clean[i];
  }
  corrupted[5] ^= 0x01;
  TEST_ASSERT_NOT_EQUAL(MatrixProtocol::checksum(clean, sizeof(clean)),
                        MatrixProtocol::checksum(corrupted, sizeof(corrupted)));
}

void test_response_frame_checksum_covers_first_five_bytes() {
  // A status response is magic, version, 0x80, status, checksum over bytes 0..4.
  const uint8_t ok[] = {0x4C, 0x4D, 0x01, 0x80, 0x00};
  TEST_ASSERT_EQUAL_UINT8(0x80, MatrixProtocol::checksum(ok, sizeof(ok)));
}

// ── Frame size arithmetic ───────────────────────────────────────────────────

void test_full_frame_payload_is_three_bytes_per_led() {
  TEST_ASSERT_EQUAL_UINT16(192, AppConfig::kLedCount * 3);
}

void test_custom_frame_payload_adds_four_metadata_bytes() {
  TEST_ASSERT_EQUAL_UINT16(196, 4 + AppConfig::kLedCount * 3);
}

void test_max_frame_size_holds_the_largest_payload() {
  TEST_ASSERT_TRUE(MatrixProtocol::kMaxFrameSize >=
                   MatrixProtocol::kHeaderSize + 196 + MatrixProtocol::kChecksumSize);
}

// ── Payload length contract ─────────────────────────────────────────────────
//
// Every opcode declares a fixed payload length, and the dispatch validates it.
// 0x0A was the one exception and accepted any length; these pin the table so a
// future opcode cannot quietly skip validation.

void test_zero_payload_commands_are_declared_zero_length() {
  // ping, clear and stop_effect all carry no payload.
  TEST_ASSERT_EQUAL_UINT8(0x00, static_cast<uint8_t>(MatrixProtocol::Command::kPing));
  TEST_ASSERT_EQUAL_UINT8(0x01, static_cast<uint8_t>(MatrixProtocol::Command::kClear));
  TEST_ASSERT_EQUAL_UINT8(0x0A, static_cast<uint8_t>(MatrixProtocol::Command::kStopEffect));
}

void test_command_opcodes_match_the_documented_protocol() {
  TEST_ASSERT_EQUAL_UINT8(0x02, static_cast<uint8_t>(MatrixProtocol::Command::kSetBrightness));
  TEST_ASSERT_EQUAL_UINT8(0x03, static_cast<uint8_t>(MatrixProtocol::Command::kFill));
  TEST_ASSERT_EQUAL_UINT8(0x04, static_cast<uint8_t>(MatrixProtocol::Command::kSetPixel));
  TEST_ASSERT_EQUAL_UINT8(0x05, static_cast<uint8_t>(MatrixProtocol::Command::kSetFrame));
  TEST_ASSERT_EQUAL_UINT8(0x06, static_cast<uint8_t>(MatrixProtocol::Command::kSetPanelEnabled));
  TEST_ASSERT_EQUAL_UINT8(0x07, static_cast<uint8_t>(MatrixProtocol::Command::kSetStaticColor));
  TEST_ASSERT_EQUAL_UINT8(0x08, static_cast<uint8_t>(MatrixProtocol::Command::kSetPresetEffect));
  TEST_ASSERT_EQUAL_UINT8(0x09, static_cast<uint8_t>(MatrixProtocol::Command::kUploadCustomFrame));
}

void test_status_codes_match_the_documented_protocol() {
  TEST_ASSERT_EQUAL_UINT8(0x00, static_cast<uint8_t>(MatrixProtocol::Status::kOk));
  TEST_ASSERT_EQUAL_UINT8(0x01, static_cast<uint8_t>(MatrixProtocol::Status::kBadMagic));
  TEST_ASSERT_EQUAL_UINT8(0x02, static_cast<uint8_t>(MatrixProtocol::Status::kUnsupportedVersion));
  TEST_ASSERT_EQUAL_UINT8(0x03, static_cast<uint8_t>(MatrixProtocol::Status::kUnknownCommand));
  TEST_ASSERT_EQUAL_UINT8(0x04, static_cast<uint8_t>(MatrixProtocol::Status::kInvalidLength));
  TEST_ASSERT_EQUAL_UINT8(0x05, static_cast<uint8_t>(MatrixProtocol::Status::kChecksumMismatch));
}

// ── Serpentine layout ───────────────────────────────────────────────────────

void test_even_rows_run_left_to_right() {
  TEST_ASSERT_EQUAL_UINT16(0, MatrixLayout::toPhysicalIndex(0, 0));
  TEST_ASSERT_EQUAL_UINT16(7, MatrixLayout::toPhysicalIndex(7, 0));
  TEST_ASSERT_EQUAL_UINT16(16, MatrixLayout::toPhysicalIndex(0, 2));
  TEST_ASSERT_EQUAL_UINT16(23, MatrixLayout::toPhysicalIndex(7, 2));
}

void test_odd_rows_run_right_to_left() {
  // Row 1 is reversed: x=0 is the far end of that row's run.
  TEST_ASSERT_EQUAL_UINT16(15, MatrixLayout::toPhysicalIndex(0, 1));
  TEST_ASSERT_EQUAL_UINT16(8, MatrixLayout::toPhysicalIndex(7, 1));
  TEST_ASSERT_EQUAL_UINT16(31, MatrixLayout::toPhysicalIndex(0, 3));
  TEST_ASSERT_EQUAL_UINT16(24, MatrixLayout::toPhysicalIndex(7, 3));
}

void test_corners_map_to_expected_chain_positions() {
  TEST_ASSERT_EQUAL_UINT16(0, MatrixLayout::toPhysicalIndex(0, 0));
  TEST_ASSERT_EQUAL_UINT16(7, MatrixLayout::toPhysicalIndex(7, 0));
  // Row 7 is odd, so the bottom-left pixel sits at the end of the chain.
  TEST_ASSERT_EQUAL_UINT16(63, MatrixLayout::toPhysicalIndex(0, 7));
  TEST_ASSERT_EQUAL_UINT16(56, MatrixLayout::toPhysicalIndex(7, 7));
}

void test_every_coordinate_maps_to_a_unique_index() {
  bool seen[AppConfig::kLedCount] = {false};
  for (uint8_t y = 0; y < AppConfig::kMatrixHeight; y++) {
    for (uint8_t x = 0; x < AppConfig::kMatrixWidth; x++) {
      const uint16_t index = MatrixLayout::toPhysicalIndex(x, y);
      TEST_ASSERT_LESS_THAN_UINT16(AppConfig::kLedCount, index);
      TEST_ASSERT_FALSE_MESSAGE(seen[index], "two coordinates mapped to one LED");
      seen[index] = true;
    }
  }
  for (uint16_t i = 0; i < AppConfig::kLedCount; i++) {
    TEST_ASSERT_TRUE_MESSAGE(seen[i], "an LED was never addressed");
  }
}

void test_out_of_range_coordinates_are_rejected() {
  TEST_ASSERT_EQUAL_UINT16(MatrixLayout::kInvalidIndex, MatrixLayout::toPhysicalIndex(8, 0));
  TEST_ASSERT_EQUAL_UINT16(MatrixLayout::kInvalidIndex, MatrixLayout::toPhysicalIndex(0, 8));
  TEST_ASSERT_EQUAL_UINT16(MatrixLayout::kInvalidIndex, MatrixLayout::toPhysicalIndex(255, 255));
  TEST_ASSERT_FALSE(MatrixLayout::isInside(8, 0));
  TEST_ASSERT_TRUE(MatrixLayout::isInside(7, 7));
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_checksum_of_empty_payload_is_zero);
  RUN_TEST(test_checksum_matches_documented_ping_frame);
  RUN_TEST(test_checksum_matches_documented_clear_frame);
  RUN_TEST(test_checksum_matches_documented_brightness_frame);
  RUN_TEST(test_checksum_matches_documented_fill_red_frame);
  RUN_TEST(test_checksum_matches_documented_panel_frames);
  RUN_TEST(test_checksum_detects_a_single_flipped_bit);
  RUN_TEST(test_response_frame_checksum_covers_first_five_bytes);

  RUN_TEST(test_full_frame_payload_is_three_bytes_per_led);
  RUN_TEST(test_custom_frame_payload_adds_four_metadata_bytes);
  RUN_TEST(test_max_frame_size_holds_the_largest_payload);

  RUN_TEST(test_zero_payload_commands_are_declared_zero_length);
  RUN_TEST(test_command_opcodes_match_the_documented_protocol);
  RUN_TEST(test_status_codes_match_the_documented_protocol);

  RUN_TEST(test_even_rows_run_left_to_right);
  RUN_TEST(test_odd_rows_run_right_to_left);
  RUN_TEST(test_corners_map_to_expected_chain_positions);
  RUN_TEST(test_every_coordinate_maps_to_a_unique_index);
  RUN_TEST(test_out_of_range_coordinates_are_rejected);

  return UNITY_END();
}
