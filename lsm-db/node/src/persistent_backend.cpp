#include "lsmdb/node/persistent_backend.h"
#ifdef LSMDB_HAS_ATISHAY_SSTABLE
#include "lsmdb/node/atishay_sstable_table_store.h"
#else
#include "lsmdb/lifecycle/reference_table_store.h"
#endif
#ifdef LSMDB_HAS_ATISHAY_SKIPLIST
#include "lsmdb/api/atishay_skiplist_memtable.h"
#include "storage/wal.h"
#else
#include "lsmdb/recovery/reference_wal.h"
#endif

#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lsmdb::node {
namespace fs = std::filesystem;

class WalAppender {
 public:
  virtual ~WalAppender() = default;
  virtual void Append(api::SequenceNumber seq, api::ValueType type, const std::string& key,
                      const std::string& value) = 0;
  virtual void DiscardThrough(api::SequenceNumber) {}
};

bool SyncParentDirectory(const std::string& file) {
#ifdef _WIN32
  (void)file;
  return true;  // MoveFileExW uses MOVEFILE_WRITE_THROUGH on this platform.
#else
  auto parent = std::filesystem::path(file).parent_path();
  if (parent.empty()) parent = ".";
  const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

#ifdef LSMDB_HAS_ATISHAY_WAL
namespace {
class AtishayWalAppender final : public WalAppender {
 public:
  explicit AtishayWalAppender(std::string path) : path_(std::move(path)) { OpenWriter(); }
  void Append(api::SequenceNumber seq, api::ValueType type, const std::string& key,
              const std::string& value) override {
    std::lock_guard<std::mutex> lock(mu_);
    lsm::WalRecord record;
    record.sequence_number = seq;
    record.version = seq;
    record.op_type = type == api::ValueType::kValue ? lsm::WalOpType::Put : lsm::WalOpType::Delete;
    record.key = key;
    record.value = value;
    const lsm::Status status = writer_->Append(record);
    if (!status.ok()) throw std::runtime_error("Atishay WAL append failed: " + status.message());
  }
  void DiscardThrough(api::SequenceNumber sequence) override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string temporary = path_ + ".checkpoint.tmp";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    if (ec) throw std::runtime_error("cannot remove stale WAL checkpoint file: " + ec.message());

    lsm::WalReader reader(path_);
    std::vector<lsm::WalRecord> records;
    bool truncated = false;
    const lsm::Status read_status = reader.ReadAll(&records, &truncated);
    if (!read_status.ok() || truncated)
      throw std::runtime_error("cannot checkpoint a damaged WAL: " + read_status.message());

    lsm::WalWriter replacement(temporary, true);
    auto status = replacement.Open();
    if (!status.ok()) throw std::runtime_error("cannot create checkpoint WAL: " + status.message());
    for (const auto& record : records) {
      if (record.sequence_number <= sequence) continue;
      status = replacement.Append(record);
      if (!status.ok()) {
        replacement.Close();
        std::filesystem::remove(temporary, ec);
        throw std::runtime_error("cannot write checkpoint WAL: " + status.message());
      }
    }
    status = replacement.Close();
    if (!status.ok()) {
      std::filesystem::remove(temporary, ec);
      throw std::runtime_error("cannot sync checkpoint WAL: " + status.message());
    }

    status = writer_->Close();
    if (!status.ok()) {
      OpenWriter();
      std::filesystem::remove(temporary, ec);
      throw std::runtime_error("cannot close original WAL for checkpoint: " + status.message());
    }
    bool replaced = false;
#ifdef _WIN32
    replaced = MoveFileExW(std::filesystem::path(temporary).c_str(), std::filesystem::path(path_).c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    replaced = ::rename(temporary.c_str(), path_.c_str()) == 0;
#endif
    if (!replaced) {
      OpenWriter();
      std::filesystem::remove(temporary, ec);
      throw std::runtime_error("cannot publish checkpoint WAL");
    }
    OpenWriter();
    if (!SyncParentDirectory(path_)) throw std::runtime_error("cannot sync WAL checkpoint directory");
  }
 private:
  void OpenWriter() {
    auto writer = std::make_unique<lsm::WalWriter>(path_, true);
    const lsm::Status status = writer->Open();
    if (!status.ok()) throw std::runtime_error("cannot open Atishay WAL: " + status.message());
    writer_ = std::move(writer);
  }
  std::string path_;
  std::mutex mu_;
  std::unique_ptr<lsm::WalWriter> writer_;
};

class AtishayWalReader final : public recovery::WalReader {
 public:
  explicit AtishayWalReader(const std::string& path) {
    lsm::WalReader reader(path);
    std::vector<lsm::WalRecord> records;
    bool truncated = false;
    const lsm::Status status = reader.ReadAll(&records, &truncated);
    if (!status.ok()) throw recovery::RecoveryError("Atishay WAL recovery failed: " + status.message());
    records_.reserve(records.size());
    for (const auto& record : records) {
      recovery::WalRecord converted;
      converted.sequence = record.sequence_number;
      converted.type = record.op_type == lsm::WalOpType::Put ? api::ValueType::kValue
                                                              : api::ValueType::kTombstone;
      converted.key = record.key;
      converted.value = record.value;
      valid_bytes_ += 8 + 25 + record.key.size() + record.value.size();
      records_.push_back(std::move(converted));
    }
    truncated_ = truncated;
  }

  recovery::WalReadResult Next(recovery::WalRecord& out) override {
    if (position_ < records_.size()) {
      out = records_[position_++];
      return recovery::WalReadResult::kRecord;
    }
    return truncated_ ? recovery::WalReadResult::kTruncatedTail : recovery::WalReadResult::kEndOfLog;
  }
  std::uint64_t ValidBytes() const override { return valid_bytes_; }

 private:
  std::vector<recovery::WalRecord> records_;
  std::size_t position_ = 0;
  std::uint64_t valid_bytes_ = 0;
  bool truncated_ = false;
};
}  // namespace
#else
namespace {
class ReferenceWalAppender final : public WalAppender {
 public:
  explicit ReferenceWalAppender(std::string path) : path_(std::move(path)) { OpenWriter(); }
  void Append(api::SequenceNumber seq, api::ValueType type, const std::string& key,
              const std::string& value) override {
    std::lock_guard<std::mutex> lock(mu_);
    writer_->Append(recovery::WalRecord{seq, type, key, value});
  }
  void DiscardThrough(api::SequenceNumber sequence) override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string temporary = path_ + ".checkpoint.tmp";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    if (ec) throw std::runtime_error("cannot remove stale reference WAL checkpoint file: " + ec.message());
    std::vector<recovery::WalRecord> keep;
    recovery::ReferenceWalReader reader(path_);
    recovery::WalRecord record;
    for (;;) {
      const auto state = reader.Next(record);
      if (state == recovery::WalReadResult::kRecord) {
        if (record.sequence > sequence) keep.push_back(record);
      } else if (state == recovery::WalReadResult::kEndOfLog) {
        break;
      } else {
        throw std::runtime_error("cannot checkpoint a damaged reference WAL");
      }
    }
    {
      recovery::ReferenceWalWriter replacement(temporary, true);
      for (const auto& item : keep) replacement.Append(item);
    }
    writer_.reset();
    bool replaced = false;
#ifdef _WIN32
    replaced = MoveFileExW(std::filesystem::path(temporary).c_str(), std::filesystem::path(path_).c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    replaced = ::rename(temporary.c_str(), path_.c_str()) == 0;
#endif
    if (!replaced) {
      OpenWriter();
      std::filesystem::remove(temporary, ec);
      throw std::runtime_error("cannot publish reference WAL checkpoint");
    }
    OpenWriter();
    if (!SyncParentDirectory(path_)) throw std::runtime_error("cannot sync reference WAL checkpoint directory");
  }
 private:
  void OpenWriter() { writer_ = std::make_unique<recovery::ReferenceWalWriter>(path_, false); }
  std::string path_;
  std::mutex mu_;
  std::unique_ptr<recovery::ReferenceWalWriter> writer_;
};
}  // namespace
#endif

std::shared_ptr<PersistentBackend> PersistentBackend::Open(const fs::path& data_dir,
                                                           lifecycle::LsmOptions options) {
  fs::create_directories(data_dir);
  const fs::path wal_path = data_dir / "wal.log";
  std::error_code stale_checkpoint_error;
  fs::remove(wal_path.string() + ".checkpoint.tmp", stale_checkpoint_error);
  if (stale_checkpoint_error)
    throw std::runtime_error("cannot remove stale WAL checkpoint: " + stale_checkpoint_error.message());
  std::shared_ptr<lifecycle::TableStore> tables;
  api::SequenceNumber table_sequence = 0;
#ifdef LSMDB_HAS_ATISHAY_SSTABLE
  auto indexed_tables = std::make_shared<AtishaySSTableTableStore>(data_dir / "tables");
  table_sequence = indexed_tables->LastSequence();
  tables = std::move(indexed_tables);
#else
  auto reference_tables = std::make_shared<lifecycle::ReferenceTableStore>(data_dir / "tables");
  table_sequence = reference_tables->LastSequence();
  tables = std::move(reference_tables);
#endif
  options.start_sequence = table_sequence;
#ifdef LSMDB_HAS_ATISHAY_SKIPLIST
  if (!options.memtable_factory)
    options.memtable_factory = [] { return std::make_shared<api::AtishaySkipListMemTable>(); };
#endif
  auto store = std::make_shared<lifecycle::LsmStore>(tables, std::move(options));

  #ifdef LSMDB_HAS_ATISHAY_WAL
  AtishayWalReader reader(wal_path.string());
  #else
  recovery::ReferenceWalReader reader(wal_path.string());
  #endif
  recovery::RecoveryStats stats = recovery::Recover(reader, *store, table_sequence);
  if (stats.dropped_torn_tail) fs::resize_file(wal_path, stats.valid_wal_bytes);

  std::shared_ptr<WalAppender> wal;
  #ifdef LSMDB_HAS_ATISHAY_WAL
  wal = std::make_shared<AtishayWalAppender>(wal_path.string());
  #else
  wal = std::make_shared<ReferenceWalAppender>(wal_path.string());
  #endif
  store->SetBeforeWriteHook([wal](api::SequenceNumber seq, api::ValueType type,
                                  const std::string& key, const std::string& value) {
    wal->Append(seq, type, key, value);
  });
  store->SetOnFlushedHook([wal](api::SequenceNumber sequence) { wal->DiscardThrough(sequence); });
  return std::shared_ptr<PersistentBackend>(
      new PersistentBackend(data_dir, std::move(tables), std::move(store), std::move(wal), stats));
}

PersistentBackend::PersistentBackend(fs::path data_dir,
                                     std::shared_ptr<lifecycle::TableStore> tables,
                                     std::shared_ptr<lifecycle::LsmStore> store,
                                     std::shared_ptr<WalAppender> wal,
                                     recovery::RecoveryStats stats)
    : data_dir_(std::move(data_dir)), tables_(std::move(tables)), store_(std::move(store)),
      wal_(std::move(wal)), recovery_stats_(stats) {}

void PersistentBackend::Put(const std::string& key, const std::string& value) {
  store_->Put(key, value);
}
std::optional<std::string> PersistentBackend::Get(const std::string& key) { return store_->Get(key); }
void PersistentBackend::Delete(const std::string& key) { store_->Delete(key); }
void PersistentBackend::Flush() { store_->Flush(); }

}  // namespace lsmdb::node
