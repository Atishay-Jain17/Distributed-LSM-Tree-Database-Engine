#pragma once

#include <filesystem>
#include <memory>

#include "lsmdb/lifecycle/lsm_store.h"
#include "lsmdb/lifecycle/table_store.h"
#include "lsmdb/recovery/recovery.h"

namespace lsmdb::node {

class WalAppender;

// Local persistent node backend: recover WAL -> LSM lifecycle -> append WAL before each write.
// On Unix builds it uses Atishay's indexed/Bloom SSTables; other builds use the portable reference store.
class PersistentBackend final : public rpc::StorageBackend {
 public:
  static std::shared_ptr<PersistentBackend> Open(const std::filesystem::path& data_dir,
                                                  lifecycle::LsmOptions options = {});

  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;
  void Flush();
  lifecycle::LsmStore& store() { return *store_; }
  const recovery::RecoveryStats& recovery_stats() const { return recovery_stats_; }
  const std::filesystem::path& data_dir() const { return data_dir_; }

 private:
  PersistentBackend(std::filesystem::path data_dir,
                    std::shared_ptr<lifecycle::TableStore> tables,
                    std::shared_ptr<lifecycle::LsmStore> store,
                    std::shared_ptr<WalAppender> wal,
                    recovery::RecoveryStats stats);

  std::filesystem::path data_dir_;
  std::shared_ptr<lifecycle::TableStore> tables_;
  std::shared_ptr<lifecycle::LsmStore> store_;
  std::shared_ptr<WalAppender> wal_;
  recovery::RecoveryStats recovery_stats_;
};

}  // namespace lsmdb::node
