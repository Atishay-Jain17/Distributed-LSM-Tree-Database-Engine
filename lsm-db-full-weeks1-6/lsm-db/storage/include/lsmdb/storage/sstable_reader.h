#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "lsmdb/storage/entry.h"
#include "lsmdb/storage/sstable_format.h"
#include "lsmdb/storage/sstable_metadata.h"

namespace lsmdb::storage {

// Read-only, thread-safe view of one SSTable (uses pread). Iterators must not outlive it.
class SSTableReader {
 public:
  // Validates the footer (magic, version, checksum, geometry). Does NOT scan the data.
  // Throws CorruptionError for a damaged file, std::runtime_error for I/O failure.
  static std::unique_ptr<SSTableReader> Open(const std::string& path);
  ~SSTableReader();
  SSTableReader(const SSTableReader&) = delete;
  SSTableReader& operator=(const SSTableReader&) = delete;

  const SSTableMetadata& metadata() const { return meta_; }
  const Footer& footer() const { return footer_; }

  // Newest version of `key` with seq <= snapshot_seq, INCLUDING tombstones (caller checks
  // entry.type). std::nullopt if this file has no such version.
  // Currently a sequential scan from the start of the file; the sparse index and Bloom
  // filter planned for Week 4 plug in here without changing this signature.
  std::optional<Entry> Get(const std::string& key, std::uint64_t snapshot_seq = kMaxSequence) const;

  // Re-reads the whole data section and checks the CRC. Throws CorruptionError on mismatch.
  void VerifyChecksum() const;

  class Iterator {
   public:
    bool Valid() const { return valid_; }
    const Entry& entry() const { return entry_; }
    void Next();  // throws CorruptionError on a malformed record

   private:
    friend class SSTableReader;
    explicit Iterator(const SSTableReader* reader);
    void Refill(std::uint64_t need);
    void Parse();

    const SSTableReader* reader_;
    std::string buf_;
    std::uint64_t buf_offset_ = 0;   // file offset of buf_[0]
    std::uint64_t next_offset_ = 0;  // file offset of the next record
    Entry entry_;
    bool valid_ = false;
  };
  // Positioned on the first entry (Valid() is false for an empty range).
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  SSTableReader() = default;
  void ReadAt(std::uint64_t offset, std::uint64_t n, std::string* out) const;

  int fd_ = -1;
  Footer footer_;
  SSTableMetadata meta_;
};

}  // namespace lsmdb::storage
