#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsmdb::storage {

// Versioned, checksummed Bloom filter stored in the optional SSTable filter section.
// False positives are allowed; false negatives are not.
class BloomFilter {
 public:
  explicit BloomFilter(std::size_t expected_items = 1);

  void Add(std::string_view key);
  bool MayContain(std::string_view key) const;
  std::size_t bit_count() const { return bit_count_; }
  std::uint32_t hash_count() const { return hash_count_; }

  std::string Serialize() const;
  static BloomFilter Deserialize(std::string_view bytes);

 private:
  BloomFilter(std::size_t bit_count, std::uint32_t hash_count, std::vector<std::uint8_t> bits);
  std::size_t bit_count_;
  std::uint32_t hash_count_;
  std::vector<std::uint8_t> bits_;
};

}  // namespace lsmdb::storage
