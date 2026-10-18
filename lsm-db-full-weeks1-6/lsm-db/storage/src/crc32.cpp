#include "lsmdb/storage/crc32.h"

#include <array>

namespace lsmdb::storage {
namespace {
std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}
}  // namespace

std::uint32_t Crc32Update(std::uint32_t crc, const char* data, std::size_t n) {
  static const std::array<std::uint32_t, 256> table = MakeTable();
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace lsmdb::storage
