#pragma once
// Hook that lets the write path be replaced without touching the RPC layer.
//
// With no WriteHandler, KvServiceImpl writes straight to the local StorageBackend
// (single-node mode, Weeks 1-5). In a cluster, the consensus layer supplies a
// WriteHandler that proposes the write through Raft and only returns OK once it is
// committed and applied. It can answer NOT_LEADER (with leader_hint) or UNAVAILABLE.
#include <string>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::rpc {

class WriteHandler {
 public:
  virtual ~WriteHandler() = default;
  virtual v1::Status Put(const std::string& key, const std::string& value) = 0;
  virtual v1::Status Delete(const std::string& key) = 0;
};

}  // namespace lsmdb::rpc
