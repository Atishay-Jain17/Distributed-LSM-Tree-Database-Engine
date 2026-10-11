#pragma once
#include <cstdint>
#include <string>

namespace lsmdb::storage {

// Summary of one finished SSTable file (returned by the writer, stored by the version set).
struct SSTableMetadata {
  std::string path;
  std::uint64_t file_size = 0;
  std::uint64_t entry_count = 0;
  std::string min_key, max_key;
  std::uint64_t min_seq = 0, max_seq = 0;
};

}  // namespace lsmdb::storage
