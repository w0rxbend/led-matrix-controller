#include "MatrixProtocol.h"

namespace MatrixProtocol {

uint8_t checksum(const uint8_t* data, uint16_t length) {
  uint8_t result = 0;

  // XOR is tiny and deterministic on an 8-bit payload. It is enough to detect
  // common framing mistakes without bringing in larger CRC code.
  for (uint16_t byteIndex = 0; byteIndex < length; byteIndex++) {
    result ^= data[byteIndex];
  }

  return result;
}

}  // namespace MatrixProtocol
