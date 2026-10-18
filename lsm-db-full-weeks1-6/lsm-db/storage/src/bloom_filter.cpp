#include "lsmdb/storage/bloom_filter.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "lsmdb/storage/coding.h"
#include "lsmdb/storage/crc32.h"

namespace lsmdb::storage {
namespace {
constexpr char kMagic[] = "LSMBLM01";
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kHashCount = 7;
constexpr std::size_t kBitsPerKey = 10;
constexpr std::size_t kMinimumBits = 64;
constexpr std::size_t kMaximumBits = 480ULL * 1024 * 1024;
constexpr std::size_t kHeaderBytes = 8 + 4 + 4 + 8;

std::uint64_t Hash(std::string_view value, std::uint64_t seed) {
  std::uint64_t h = 14695981039346656037ULL ^ seed;
  for (unsigned char c : value) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  // Final avalanche prevents low-entropy keys from clustering under double hashing.
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  return h ^ (h >> 31);
}

}  // namespace

BloomFilter::BloomFilter(std::size_t expected_items)
    : bit_count_(std::clamp(expected_items, std::size_t{1}, kMaximumBits / kBitsPerKey) * kBitsPerKey),
      hash_count_(kHashCount), bits_((bit_count_ + 7) / 8, 0) {
  bit_count_ = std::max(bit_count_, kMinimumBits);
  bits_.resize((bit_count_ + 7) / 8, 0);
}

BloomFilter::BloomFilter(std::size_t bit_count, std::uint32_t hash_count,
                         std::vector<std::uint8_t> bits)
    : bit_count_(bit_count), hash_count_(hash_count), bits_(std::move(bits)) {}

void BloomFilter::Add(std::string_view key) {
  const std::uint64_t h1 = Hash(key, 0x9e3779b97f4a7c15ULL);
  const std::uint64_t h2 = Hash(key, 0xd6e8feb86659fd93ULL) | 1ULL;
  for (std::uint32_t i = 0; i < hash_count_; ++i) {
    const std::size_t bit = static_cast<std::size_t>((h1 + i * h2) % bit_count_);
    bits_[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
  }
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (bit_count_ == 0 || bits_.empty() || hash_count_ == 0) return true;
  const std::uint64_t h1 = Hash(key, 0x9e3779b97f4a7c15ULL);
  const std::uint64_t h2 = Hash(key, 0xd6e8feb86659fd93ULL) | 1ULL;
  for (std::uint32_t i = 0; i < hash_count_; ++i) {
    const std::size_t bit = static_cast<std::size_t>((h1 + i * h2) % bit_count_);
    if ((bits_[bit / 8] & static_cast<std::uint8_t>(1U << (bit % 8))) == 0) return false;
  }
  return true;
}

std::string BloomFilter::Serialize() const {
  if (bit_count_ == 0 || bits_.size() != (bit_count_ + 7) / 8 || hash_count_ == 0)
    throw std::logic_error("cannot serialize invalid Bloom filter");
  std::string out(kMagic, sizeof(kMagic) - 1);
  PutFixed32(&out, kVersion);
  PutFixed32(&out, hash_count_);
  PutFixed64(&out, bit_count_);
  out.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
  PutFixed32(&out, Crc32Update(0, out.data(), out.size()));
  return out;
}

BloomFilter BloomFilter::Deserialize(std::string_view bytes) {
  if (bytes.size() < kHeaderBytes + 1 + 4 || bytes.substr(0, 8) != std::string_view(kMagic, 8))
    throw std::runtime_error("invalid Bloom filter header");
  const auto version = DecodeFixed32(bytes.data() + 8);
  const auto hashes = DecodeFixed32(bytes.data() + 12);
  const auto bit_count_u64 = DecodeFixed64(bytes.data() + 16);
  if (version != kVersion || hashes == 0 || hashes > 32 || bit_count_u64 < kMinimumBits ||
      bit_count_u64 > kMaximumBits || bit_count_u64 > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error("unsupported or invalid Bloom filter parameters");
  const auto bit_count = static_cast<std::size_t>(bit_count_u64);
  const std::size_t bit_bytes = (bit_count + 7) / 8;
  const std::size_t logical_size = kHeaderBytes + bit_bytes + 4;
  if (logical_size > bytes.size()) throw std::runtime_error("truncated Bloom filter bits");
  for (std::size_t i = logical_size; i < bytes.size(); ++i)
    if (bytes[i] != '\0') throw std::runtime_error("nonzero Bloom filter padding");
  const auto expected_crc = DecodeFixed32(bytes.data() + kHeaderBytes + bit_bytes);
  if (Crc32Update(0, bytes.data(), kHeaderBytes + bit_bytes) != expected_crc)
    throw std::runtime_error("Bloom filter checksum mismatch");
  std::vector<std::uint8_t> bits(bit_bytes);
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data() + kHeaderBytes), bit_bytes, bits.begin());
  return BloomFilter(bit_count, hashes, std::move(bits));
}

}  // namespace lsmdb::storage
