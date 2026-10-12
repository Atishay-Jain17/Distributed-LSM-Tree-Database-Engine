#pragma once
#include <cstddef>
#include <cstdint>

namespace lsmdb::storage {

struct CompactionOptions {
  std::size_t l0_file_trigger = 4;                          // compact L0 when it has this many files
  std::uint64_t level1_max_bytes = 10ull * 1024 * 1024;     // size budget of L1
  std::uint64_t level_multiplier = 10;                      // each deeper level may be 10x larger
  std::uint64_t target_file_bytes = 2ull * 1024 * 1024;     // output files roll over near this size
};

}  // namespace lsmdb::storage
