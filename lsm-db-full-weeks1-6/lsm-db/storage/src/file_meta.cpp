#include "lsmdb/storage/file_meta.h"

#include <filesystem>

namespace lsmdb::storage {

FileMeta::~FileMeta() {
  if (obsolete_) {
    std::error_code ec;
    std::filesystem::remove(meta.path, ec);  // best effort; a leftover file is harmless
  }
}

std::shared_ptr<SSTableReader> FileMeta::Reader() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!reader_) reader_ = SSTableReader::Open(meta.path);
  return reader_;
}

}  // namespace lsmdb::storage
