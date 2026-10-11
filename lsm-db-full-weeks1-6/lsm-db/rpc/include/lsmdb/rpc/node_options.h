#pragma once
// Minimal per-node settings needed by the RPC layer.
// NOTE: Atishay's Week 1 configuration layer owns the full config (data dir,
// storage settings). When it lands, build a NodeOptions from it; this struct
// only covers identity + network.
#include <cstdint>
#include <string>
#include <vector>

#include "lsmdb/v1/common.pb.h"

namespace lsmdb::rpc {

struct NodeOptions {
  std::uint32_t node_id = 0;                // 1-based
  std::string advertise_host = "127.0.0.1"; // address other nodes use to reach this one
  std::uint32_t port = 0;                   // listen port; 0 = pick a free port (tests only)
  std::vector<v1::NodeInfo> peers;          // the OTHER nodes
};

// Parses "2=host2:50052,3=host3:50053". Empty string -> empty list.
// Throws std::invalid_argument on malformed input, bad ids/ports or duplicates.
std::vector<v1::NodeInfo> ParsePeers(const std::string& spec);

// Throws std::invalid_argument if options are unusable.
void ValidateNodeOptions(const NodeOptions& options, bool allow_ephemeral_port = false);

}  // namespace lsmdb::rpc
