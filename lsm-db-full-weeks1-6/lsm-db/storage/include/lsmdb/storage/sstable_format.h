#pragma once
// ---------------------------------------------------------------------------
// SSTable file format, version 1 (Week 3). Written once, never modified.
//
//   +----------------------------+  offset 0
//   | DATA SECTION               |  records back to back, sorted by EntryLess
//   | zero padding to 4096       |
//   +----------------------------+
//   | (reserved) INDEX SECTION   |  Atishay, Week 4: sparse index. index_offset/size = 0 today
//   | (reserved) FILTER SECTION  |  Sarthak, Week 4: Bloom filter. filter_offset/size = 0 today
//   |  each padded to 4096       |  (new sections go BETWEEN data and footer)
//   +----------------------------+
//   | FOOTER (exactly 4096 bytes)|  always the LAST page of the file
//   +----------------------------+
//
// RECORD (little-endian):
//   u32 key_len | u32 value_len | u64 seq | u8 type(0=put,1=delete) | key | value
//
// FOOTER (little-endian):
//   0   8   magic "LSMSST01"
//   8   4   format version (1)
//   12  4   flags (0)
//   16  8   data_size (bytes of records, excluding padding)
//   24  8   entry_count
//   32  8   min_seq
//   40  8   max_seq
//   48  8   index_offset     56 8 index_size
//   64  8   filter_offset    72 8 filter_size
//   80  4   data_crc32 (over the data_size record bytes)
//   84  4   min_key_len      88 4 max_key_len     92 4 reserved
//   96  ..  min_key bytes, then max_key bytes
//   4092 4  footer_crc32 (over bytes 0..4091)
//
// O_DIRECT LAYOUT: every section starts at a multiple of 4096 and has a length
// that is a multiple of 4096, and the file size is a multiple of 4096. Sarthak's
// direct-I/O layer can therefore read any section with aligned offsets/lengths.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace lsmdb::storage {

struct CorruptionError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline constexpr std::size_t kBlockAlignment = 4096;
inline constexpr std::size_t kFooterSize = 4096;
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kRecordHeaderSize = 4 + 4 + 8 + 1;
inline constexpr std::size_t kMaxSSTableKeyBytes = 1024;
inline constexpr std::size_t kMaxSSTableValueBytes = 16 * 1024 * 1024;

inline std::size_t AlignUp(std::size_t n, std::size_t a = kBlockAlignment) {
  return (n + a - 1) / a * a;
}

struct Footer {
  std::uint64_t data_size = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t min_seq = 0;
  std::uint64_t max_seq = 0;
  std::uint64_t index_offset = 0, index_size = 0;
  std::uint64_t filter_offset = 0, filter_size = 0;
  std::uint32_t data_crc = 0;
  std::string min_key, max_key;
};

std::string EncodeFooter(const Footer& footer);  // exactly kFooterSize bytes
Footer DecodeFooter(const char* page);           // throws CorruptionError

}  // namespace lsmdb::storage
