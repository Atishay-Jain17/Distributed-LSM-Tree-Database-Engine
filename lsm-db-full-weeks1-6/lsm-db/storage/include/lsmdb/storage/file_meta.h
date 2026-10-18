#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "lsmdb/storage/sstable_metadata.h"
#include "lsmdb/storage/sstable_reader.h"

namespace lsmdb::storage {

// One live SSTable file, shared by every Version that contains it.
//
// FILE LIFETIME RULE (the answer to "when can compaction delete a file?"):
// compaction only MARKS replaced files obsolete. The file is physically deleted in this
// object's destructor, i.e. when the LAST Version/reader holding a shared_ptr<FileMeta>
// lets go. A read that started before the compaction therefore never loses its files.
class FileMeta {
 public:
  FileMeta(std::uint64_t number, int level, SSTableMetadata meta)
      : number(number), level(level), meta(std::move(meta)) {}
  ~FileMeta();
  FileMeta(const FileMeta&) = delete;
  FileMeta& operator=(const FileMeta&) = delete;

  const std::uint64_t number;
  const int level;
  const SSTableMetadata meta;

  void MarkObsolete() { obsolete_ = true; }
  bool obsolete() const { return obsolete_; }

  // Opened lazily, once, and cached. Thread-safe.
  std::shared_ptr<SSTableReader> Reader() const;

 private:
  std::atomic<bool> obsolete_{false};
  mutable std::mutex mu_;
  mutable std::shared_ptr<SSTableReader> reader_;
};

}  // namespace lsmdb::storage
