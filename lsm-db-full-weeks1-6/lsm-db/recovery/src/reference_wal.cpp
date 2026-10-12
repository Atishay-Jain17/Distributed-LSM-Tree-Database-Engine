#include "lsmdb/recovery/reference_wal.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace lsmdb::recovery {
namespace {
void Put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xff)); }
void Put64(std::string& s, std::uint64_t v) { for (int i = 0; i < 8; ++i) s.push_back(char((v >> (8 * i)) & 0xff)); }
std::uint32_t Get32(const unsigned char* p) { std::uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
std::uint64_t Get64(const unsigned char* p) { std::uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }
constexpr std::size_t kHeader = 8;
constexpr std::size_t kFixedPayload = 8 + 1 + 4 + 4;
}  // namespace

std::uint32_t Crc32(const unsigned char* data, std::size_t n) {
  std::uint32_t c = 0xffffffffu;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}

ReferenceWalWriter::ReferenceWalWriter(const std::string& path, bool truncate)
    : f_(std::fopen(path.c_str(), truncate ? "wb" : "ab")) {
  if (!f_) throw std::runtime_error("cannot open WAL for writing: " + path);
  if (!truncate) { std::fseek(f_, 0, SEEK_END); size_ = static_cast<std::uint64_t>(std::ftell(f_)); }
}
ReferenceWalWriter::~ReferenceWalWriter() { if (f_) std::fclose(f_); }

std::uint64_t ReferenceWalWriter::Append(const WalRecord& rec) {
  std::string payload;
  Put64(payload, rec.sequence);
  payload.push_back(char(rec.type));
  Put32(payload, std::uint32_t(rec.key.size()));
  Put32(payload, std::uint32_t(rec.value.size()));
  payload += rec.key;
  payload += rec.value;
  std::string out;
  Put32(out, std::uint32_t(payload.size()));
  Put32(out, Crc32(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()));
  out += payload;
  if (std::fwrite(out.data(), 1, out.size(), f_) != out.size() || std::fflush(f_) != 0)
    throw std::runtime_error("WAL write failed");
  size_ += out.size();
  return size_;
}

ReferenceWalReader::ReferenceWalReader(const std::string& path) {
  if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
    unsigned char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data_.insert(data_.end(), buf, buf + n);
    std::fclose(f);
  }
}

WalReadResult ReferenceWalReader::Next(WalRecord& out) {
  if (final_ != WalReadResult::kRecord) return final_;
  const std::uint64_t remaining = data_.size() - pos_;
  if (remaining == 0) return final_ = WalReadResult::kEndOfLog;
  if (remaining < kHeader) return final_ = WalReadResult::kTruncatedTail;
  const unsigned char* p = data_.data() + pos_;
  const std::uint32_t len = Get32(p), crc = Get32(p + 4);
  if (remaining < kHeader + std::uint64_t(len)) return final_ = WalReadResult::kTruncatedTail;
  const bool is_last = remaining == kHeader + std::uint64_t(len);
  const unsigned char* payload = p + kHeader;
  bool ok = len >= kFixedPayload && Crc32(payload, len) == crc;
  if (ok) {
    const std::uint32_t klen = Get32(payload + 9), vlen = Get32(payload + 13);
    ok = std::uint64_t(kFixedPayload) + klen + vlen == len && (payload[8] == 1 || payload[8] == 2);
    if (ok) {
      out.sequence = Get64(payload);
      out.type = static_cast<api::ValueType>(payload[8]);
      out.key.assign(reinterpret_cast<const char*>(payload + kFixedPayload), klen);
      out.value.assign(reinterpret_cast<const char*>(payload + kFixedPayload + klen), vlen);
    }
  }
  if (!ok) {
    // Damaged last record = torn append (tolerated). Damage with more data after it = corruption.
    return final_ = is_last ? WalReadResult::kTruncatedTail : WalReadResult::kCorrupt;
  }
  pos_ += kHeader + len;
  valid_ = pos_;
  return WalReadResult::kRecord;
}

}  // namespace lsmdb::recovery
