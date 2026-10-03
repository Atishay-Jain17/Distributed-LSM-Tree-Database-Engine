#pragma once
// ---------------------------------------------------------------------------
// INTEGRATION CONTRACT (temporary, owned jointly with Atishay/Suhani).
//
// The RPC layer must not depend on a concrete storage engine. This is the
// smallest interface it needs. When the real storage API (Week 1, Atishay) is
// merged, add a thin adapter class that implements StorageBackend by calling it
// and pass that to NodeServer. Nothing else in rpc/ needs to change.
//
// Error model: implementations throw std::exception on unexpected failure; the
// RPC layer converts that into STATUS_CODE_INTERNAL. "Key missing" is NOT an
// error: Get() returns std::nullopt.
// Implementations must be thread-safe: gRPC calls them from many threads.
// ---------------------------------------------------------------------------
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace lsmdb::rpc {

class StorageBackend {
 public:
  virtual ~StorageBackend() = default;
  virtual void Put(const std::string& key, const std::string& value) = 0;
  virtual std::optional<std::string> Get(const std::string& key) = 0;
  virtual void Delete(const std::string& key) = 0;  // idempotent
};

// Placeholder used for the Week 1 smoke test and RPC tests. NOT the database.
class InMemoryBackend : public StorageBackend {
 public:
  void Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  void Delete(const std::string& key) override;

 private:
  std::mutex mu_;
  std::map<std::string, std::string> data_;
};

}  // namespace lsmdb::rpc
