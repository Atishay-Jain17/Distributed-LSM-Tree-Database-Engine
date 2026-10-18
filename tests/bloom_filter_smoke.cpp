#include <iostream>
#include <stdexcept>
#include <string>

#include "lsmdb/storage/bloom_filter.h"
#include "lsmdb/storage/sstable_format.h"

int main() {
  try {
    lsmdb::storage::BloomFilter filter(1000);
    for (int i = 0; i < 1000; ++i) filter.Add("key-" + std::to_string(i));
    std::string bytes = filter.Serialize();
    bytes.resize(lsmdb::storage::AlignUp(bytes.size()), '\0');
    const auto restored = lsmdb::storage::BloomFilter::Deserialize(bytes);
    for (int i = 0; i < 1000; ++i) {
      if (!restored.MayContain("key-" + std::to_string(i)))
        throw std::runtime_error("Bloom filter produced a false negative");
    }
    bool rejected_miss = false;
    for (int i = 0; i < 1000 && !rejected_miss; ++i)
      rejected_miss = !restored.MayContain("absent-key-" + std::to_string(i));
    if (!rejected_miss) throw std::runtime_error("Bloom filter rejected none of the absent keys");
    bytes[24] ^= 0x01;
    bool corruption_rejected = false;
    try { (void)lsmdb::storage::BloomFilter::Deserialize(bytes); }
    catch (const std::runtime_error&) { corruption_rejected = true; }
    if (!corruption_rejected) throw std::runtime_error("Bloom filter checksum failure was accepted");
    std::cout << "PASS: Bloom filter round-trip, no false negatives, definite miss and corruption check\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
