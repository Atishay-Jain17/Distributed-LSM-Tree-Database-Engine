#pragma once
#include <cstdint>
#include <string>

#include "lsmdb/storage/entry.h"
#include "lsmdb/storage/sstable_metadata.h"

namespace lsmdb::storage {

// Writes one SSTable. Entries must be added in EntryLess order (strictly increasing).
//
// Crash safety: data goes to "<path>.tmp"; Finish() pads, writes the footer, fsyncs the
// file, renames it to <path> and fsyncs the directory. A reader therefore sees either
// no file or a complete one. If the writer is destroyed without Finish(), the temp file
// is removed.
class SSTableWriter {
 public:
  explicit SSTableWriter(std::string path);  // throws std::runtime_error on I/O failure
  ~SSTableWriter();
  SSTableWriter(const SSTableWriter&) = delete;
  SSTableWriter& operator=(const SSTableWriter&) = delete;

  void Add(const Entry& entry);  // throws std::invalid_argument on order/size violation

  std::uint64_t entry_count() const { return entry_count_; }
  std::uint64_t data_bytes() const { return data_bytes_; }  // grows as entries are added

  // Throws std::logic_error if no entries were added.
  SSTableMetadata Finish();

 private:
  void FlushBuffer();
  void WriteAll(const char* data, std::size_t n);

  std::string path_, tmp_path_;
  int fd_ = -1;
  bool finished_ = false;
  std::string buffer_;
  std::uint64_t data_bytes_ = 0, entry_count_ = 0;
  std::uint32_t crc_ = 0;
  std::uint64_t min_seq_ = 0, max_seq_ = 0;
  std::string min_key_, last_key_;
  std::uint64_t last_seq_ = 0;
};

}  // namespace lsmdb::storage
