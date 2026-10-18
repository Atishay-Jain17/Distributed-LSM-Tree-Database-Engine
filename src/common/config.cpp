#include "common/config.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace lsm {

namespace {

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool ParseBool(const std::string& str, bool default_val = false) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        return false;
    }
    return default_val;
}

} // namespace

NodeConfig NodeConfig::Default() {
    return NodeConfig{};
}

Status NodeConfig::Validate() const {
    if (node_id.empty()) {
        return Status::InvalidArgument("Node ID cannot be empty");
    }
    if (storage.data_dir.empty()) {
        return Status::InvalidArgument("Storage data directory cannot be empty");
    }
    if (network.client_port <= 0 || network.client_port > 65535) {
        return Status::InvalidArgument("Client port must be between 1 and 65535");
    }
    if (network.peer_port <= 0 || network.peer_port > 65535) {
        return Status::InvalidArgument("Peer port must be between 1 and 65535");
    }
    if (network.metrics_port <= 0 || network.metrics_port > 65535) {
        return Status::InvalidArgument("Metrics port must be between 1 and 65535");
    }
    if (network.client_port == network.peer_port ||
        network.client_port == network.metrics_port ||
        network.peer_port == network.metrics_port) {
        return Status::InvalidArgument("Ports must be distinct to avoid address collisions");
    }
    return Status::Ok();
}

Status NodeConfig::LoadFromFile(const std::string& filepath, NodeConfig& out_config) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return Status::IOError("Could not open configuration file: " + filepath);
    }

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        line_number++;
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        size_t eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            return Status::InvalidArgument("Invalid config format at line " + 
                                          std::to_string(line_number) + ": expected key=value");
        }

        std::string key = Trim(trimmed.substr(0, eq_pos));
        std::string value = Trim(trimmed.substr(eq_pos + 1));

        if (key == "node.id" || key == "node_id") {
            out_config.node_id = value;
        } else if (key == "storage.data_dir" || key == "data_dir") {
            out_config.storage.data_dir = value;
        } else if (key == "storage.max_memtable_size" || key == "max_memtable_size") {
            try {
                out_config.storage.max_memtable_size = std::stoull(value);
            } catch (...) {
                return Status::InvalidArgument("Invalid max_memtable_size: " + value);
            }
        } else if (key == "storage.sync_wal" || key == "sync_wal") {
            out_config.storage.sync_wal = ParseBool(value, true);
        } else if (key == "network.client_port" || key == "client_port") {
            try {
                out_config.network.client_port = std::stoi(value);
            } catch (...) {
                return Status::InvalidArgument("Invalid client_port: " + value);
            }
        } else if (key == "network.peer_port" || key == "peer_port") {
            try {
                out_config.network.peer_port = std::stoi(value);
            } catch (...) {
                return Status::InvalidArgument("Invalid peer_port: " + value);
            }
        } else if (key == "network.metrics_port" || key == "metrics_port") {
            try {
                out_config.network.metrics_port = std::stoi(value);
            } catch (...) {
                return Status::InvalidArgument("Invalid metrics_port: " + value);
            }
        } else {
            // Unrecognized keys are ignored or logged
        }
    }

    return out_config.Validate();
}

Status NodeConfig::ParseCommandLine(int argc, char* argv[], NodeConfig& out_config) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto next_arg = [&]() -> const char* {
            if (i + 1 < argc) {
                return argv[++i];
            }
            return nullptr;
        };

        if (arg == "--config") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for --config");
            Status s = LoadFromFile(val, out_config);
            if (!s.ok()) return s;
        } else if (arg == "--node_id" || arg == "--node-id") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for " + arg);
            out_config.node_id = val;
        } else if (arg == "--data_dir" || arg == "--data-dir") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for " + arg);
            out_config.storage.data_dir = val;
        } else if (arg == "--client_port" || arg == "--client-port") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for " + arg);
            try {
                out_config.network.client_port = std::stoi(val);
            } catch (...) {
                return Status::InvalidArgument("Invalid port for " + arg);
            }
        } else if (arg == "--peer_port" || arg == "--peer-port") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for " + arg);
            try {
                out_config.network.peer_port = std::stoi(val);
            } catch (...) {
                return Status::InvalidArgument("Invalid port for " + arg);
            }
        } else if (arg == "--metrics_port" || arg == "--metrics-port") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for " + arg);
            try {
                out_config.network.metrics_port = std::stoi(val);
            } catch (...) {
                return Status::InvalidArgument("Invalid port for " + arg);
            }
        } else if (arg == "--sync_wal" || arg == "--sync-wal") {
            const char* val = next_arg();
            if (!val) return Status::InvalidArgument("Missing value for " + arg);
            out_config.storage.sync_wal = ParseBool(val, true);
        }
    }

    return out_config.Validate();
}

std::string NodeConfig::ToString() const {
    std::ostringstream oss;
    oss << "NodeConfig {\n"
        << "  node_id: " << node_id << "\n"
        << "  storage: {\n"
        << "    data_dir: " << storage.data_dir << ",\n"
        << "    max_memtable_size: " << storage.max_memtable_size << " bytes,\n"
        << "    sync_wal: " << (storage.sync_wal ? "true" : "false") << "\n"
        << "  },\n"
        << "  network: {\n"
        << "    client_port: " << network.client_port << ",\n"
        << "    peer_port: " << network.peer_port << ",\n"
        << "    metrics_port: " << network.metrics_port << "\n"
        << "  }\n"
        << "}";
    return oss.str();
}

} // namespace lsm
