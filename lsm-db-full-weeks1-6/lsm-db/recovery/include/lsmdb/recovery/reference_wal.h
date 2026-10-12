#pragma once
// STAND-IN WAL used only to test recovery until Atishay's WAL lands. Simple, correct, slow.
// Record: [u32 payload_len][u32 crc32(payload)][payload]
// payload: u64 seq | u8 type | u32 key_len | u32 value_len | key | value   (little-endian)
#include <cstdint>
#include <string>
#include <vector>

#include "lsmdb/recovery/wal_reader.h"

namespace lsmdb::recovery {

class ReferenceWalWriter {
 public:
  explicit ReferenceWalWriter(const std::string& path, bool truncate = true);
  ~ReferenceWalWriter();
  ReferenceWalWriter(const ReferenceWalWriter&) = delete;
  ReferenceWalWriter& operator=(const ReferenceWalWriter&) = delete;
  // Appends and flushes one record; returns the file size after the append.
  std::uint64_t Append(const WalRecord& rec);

 private:
  std::FILE* f_;
  std::uint64_t size_ = 0;
};

class ReferenceWalReader : public WalReader {
 public:
  explicit ReferenceWalReader(const std::string& path);  // missing file == empty log
  WalReadResult Next(WalRecord& out) override;
  std::uint64_t ValidBytes() const override { return valid_; }

 private:
  std::vector<unsigned char> data_;
  std::uint64_t pos_ = 0, valid_ = 0;
  WalReadResult final_ = WalReadResult::kRecord;
};

std::uint32_t Crc32(const unsigned char* data, std::size_t n);

}  // namespace lsmdb::recovery
