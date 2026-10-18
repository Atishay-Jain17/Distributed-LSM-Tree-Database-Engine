#pragma once

#include <string>
#include <cstdint>
#include "common/status.h"

namespace lsm {

struct StorageConfig {
    std::string data_dir = "./data";
    size_t max_memtable_size = 64 * 1024 * 1024; // 64 MB (configured for future weeks)
    bool sync_wal = true;
};

struct NetworkConfig {
    int client_port = 50051;
    int peer_port = 50052;
    int metrics_port = 9090;
};

struct NodeConfig {
    std::string node_id = "node-1";
    StorageConfig storage;
    NetworkConfig network;

    static NodeConfig Default();
    static Status LoadFromFile(const std::string& filepath, NodeConfig& out_config);
    static Status ParseCommandLine(int argc, char* argv[], NodeConfig& out_config);

    [[nodiscard]] Status Validate() const;
    [[nodiscard]] std::string ToString() const;
};

} // namespace lsm
