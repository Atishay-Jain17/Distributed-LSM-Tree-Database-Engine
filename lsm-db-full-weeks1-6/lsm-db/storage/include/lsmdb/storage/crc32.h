#pragma once
#include <cstddef>
#include <cstdint>

namespace lsmdb::storage {

// Standard CRC-32 (IEEE 802.3). Chainable: Crc32Update(Crc32Update(0, a), b) == CRC of a+b.
std::uint32_t Crc32Update(std::uint32_t crc, const char* data, std::size_t n);

}  // namespace lsmdb::storage
