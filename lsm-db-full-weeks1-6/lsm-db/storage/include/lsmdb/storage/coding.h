#pragma once
// Little-endian fixed-width encoding helpers (explicit byte order, so files are portable).
#include <cstdint>
#include <string>

namespace lsmdb::storage {

inline void EncodeFixed32(char* dst, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}
inline void EncodeFixed64(char* dst, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}
inline std::uint32_t DecodeFixed32(const char* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}
inline std::uint64_t DecodeFixed64(const char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}
inline void PutFixed32(std::string* dst, std::uint32_t v) {
  char b[4];
  EncodeFixed32(b, v);
  dst->append(b, 4);
}
inline void PutFixed64(std::string* dst, std::uint64_t v) {
  char b[8];
  EncodeFixed64(b, v);
  dst->append(b, 8);
}

}  // namespace lsmdb::storage
