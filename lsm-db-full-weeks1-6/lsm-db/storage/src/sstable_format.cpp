#include "lsmdb/storage/sstable_format.h"

#include <cstring>

#include "lsmdb/storage/coding.h"
#include "lsmdb/storage/crc32.h"

namespace lsmdb::storage {
namespace {
constexpr char kMagic[8] = {'L', 'S', 'M', 'S', 'S', 'T', '0', '1'};
constexpr std::size_t kKeysOffset = 96;
constexpr std::size_t kCrcOffset = kFooterSize - 4;
}  // namespace

std::string EncodeFooter(const Footer& f) {
  if (f.min_key.size() > kMaxSSTableKeyBytes || f.max_key.size() > kMaxSSTableKeyBytes) {
    throw std::invalid_argument("footer key too long");
  }
  std::string page(kFooterSize, '\0');
  char* p = page.data();
  std::memcpy(p, kMagic, 8);
  EncodeFixed32(p + 8, kFormatVersion);
  EncodeFixed32(p + 12, 0);
  EncodeFixed64(p + 16, f.data_size);
  EncodeFixed64(p + 24, f.entry_count);
  EncodeFixed64(p + 32, f.min_seq);
  EncodeFixed64(p + 40, f.max_seq);
  EncodeFixed64(p + 48, f.index_offset);
  EncodeFixed64(p + 56, f.index_size);
  EncodeFixed64(p + 64, f.filter_offset);
  EncodeFixed64(p + 72, f.filter_size);
  EncodeFixed32(p + 80, f.data_crc);
  EncodeFixed32(p + 84, static_cast<std::uint32_t>(f.min_key.size()));
  EncodeFixed32(p + 88, static_cast<std::uint32_t>(f.max_key.size()));
  std::memcpy(p + kKeysOffset, f.min_key.data(), f.min_key.size());
  std::memcpy(p + kKeysOffset + f.min_key.size(), f.max_key.data(), f.max_key.size());
  EncodeFixed32(p + kCrcOffset, Crc32Update(0, p, kCrcOffset));
  return page;
}

Footer DecodeFooter(const char* p) {
  if (std::memcmp(p, kMagic, 8) != 0) throw CorruptionError("bad SSTable magic");
  if (DecodeFixed32(p + kCrcOffset) != Crc32Update(0, p, kCrcOffset)) {
    throw CorruptionError("footer checksum mismatch");
  }
  if (DecodeFixed32(p + 8) != kFormatVersion) throw CorruptionError("unsupported SSTable version");
  Footer f;
  f.data_size = DecodeFixed64(p + 16);
  f.entry_count = DecodeFixed64(p + 24);
  f.min_seq = DecodeFixed64(p + 32);
  f.max_seq = DecodeFixed64(p + 40);
  f.index_offset = DecodeFixed64(p + 48);
  f.index_size = DecodeFixed64(p + 56);
  f.filter_offset = DecodeFixed64(p + 64);
  f.filter_size = DecodeFixed64(p + 72);
  f.data_crc = DecodeFixed32(p + 80);
  std::uint32_t min_len = DecodeFixed32(p + 84), max_len = DecodeFixed32(p + 88);
  if (min_len > kMaxSSTableKeyBytes || max_len > kMaxSSTableKeyBytes ||
      kKeysOffset + min_len + max_len > kCrcOffset) {
    throw CorruptionError("footer key lengths out of range");
  }
  f.min_key.assign(p + kKeysOffset, min_len);
  f.max_key.assign(p + kKeysOffset + min_len, max_len);
  return f;
}

}  // namespace lsmdb::storage
