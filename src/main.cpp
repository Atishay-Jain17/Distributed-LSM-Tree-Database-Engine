#include <iostream>
#include <string>
#include <sstream>
#include "common/config.h"
#include "common/status.h"
#include "storage/basic_storage_engine.h"

using namespace lsm;

void PrintUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --config <path>             Load configuration from file\n"
              << "  --node_id <id>              Set the node ID (default: node-1)\n"
              << "  --data_dir <path>           Set data directory (default: ./data)\n"
              << "  --client_port <port>        Set client-facing port (default: 50051)\n"
              << "  --peer_port <port>          Set inter-node peer port (default: 50052)\n"
              << "  --metrics_port <port>       Set metrics telemetry port (default: 9090)\n"
              << "  --sync_wal <true|false>     Enable/disable synchronous WAL (default: true)\n"
              << "  --help                      Display this help message\n";
}

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n"
              << " Distributed LSM-Tree Database Engine (Node Daemon)     \n"
              << " Week 1 Storage & Node Foundation                       \n"
              << "========================================================\n";

    // 1. Check for --help
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    // 2. Load and parse configuration
    NodeConfig config = NodeConfig::Default();
    Status config_status = NodeConfig::ParseCommandLine(argc, argv, config);
    if (!config_status.ok()) {
        std::cerr << "Configuration Error: " << config_status.ToString() << std::endl;
        return 1;
    }

    std::cout << "Starting node with configuration:\n" << config.ToString() << std::endl;

    // 3. Initialize and open storage engine
    BasicStorageEngine storage(config.storage);
    Status open_status = storage.Open();
    if (!open_status.ok()) {
        std::cerr << "Storage Initialization Error: " << open_status.ToString() << std::endl;
        return 1;
    }

    std::cout << "\nStorage engine initialized and ready.\n"
              << "Interactive CLI active. Type 'HELP' for available commands, 'EXIT' to quit.\n"
              << "--------------------------------------------------------\n";

    std::string line;
    std::cout << "lsm (" << config.node_id << ")> " << std::flush;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd.empty()) {
            std::cout << "lsm (" << config.node_id << ")> " << std::flush;
            continue;
        }

        for (auto& c : cmd) c = static_cast<char>(std::toupper(c));

        if (cmd == "EXIT" || cmd == "QUIT") {
            break;
        } else if (cmd == "PUT") {
            std::string key, val;
            iss >> key;
            std::string rest;
            std::getline(iss, rest);
            // Trim leading space of rest
            size_t first = rest.find_first_not_of(" \t");
            if (first != std::string::npos) {
                val = rest.substr(first);
            }
            if (key.empty()) {
                std::cout << "Error: PUT requires <key> <value>\n";
            } else {
                Status s = storage.Put(key, val);
                std::cout << (s.ok() ? "OK" : s.ToString()) << "\n";
            }
        } else if (cmd == "GET") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Error: GET requires <key>\n";
            } else {
                std::string val;
                Status s = storage.Get(key, &val);
                if (s.ok()) {
                    std::cout << "VALUE: \"" << val << "\"\n";
                } else {
                    std::cout << s.ToString() << "\n";
                }
            }
        } else if (cmd == "DELETE" || cmd == "DEL") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Error: DELETE requires <key>\n";
            } else {
                Status s = storage.Delete(key);
                std::cout << (s.ok() ? "OK" : s.ToString()) << "\n";
            }
        } else if (cmd == "CONTAINS") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Error: CONTAINS requires <key>\n";
            } else {
                std::cout << (storage.Contains(key) ? "TRUE" : "FALSE") << "\n";
            }
        } else if (cmd == "INFO") {
            std::cout << "Node ID: " << config.node_id << "\n"
                      << "Data Directory: " << config.storage.data_dir << "\n"
                      << "Entry Count: " << storage.EntryCount() << "\n"
                      << "Approximate Storage Size: " << storage.ApproximateSize() << " bytes\n";
        } else if (cmd == "HELP") {
            std::cout << "Commands:\n"
                      << "  PUT <key> <value>   - Store key-value pair\n"
                      << "  GET <key>           - Retrieve value for key\n"
                      << "  DELETE <key>        - Delete key\n"
                      << "  CONTAINS <key>      - Check key existence\n"
                      << "  INFO                - Display node and storage stats\n"
                      << "  HELP                - Show command list\n"
                      << "  EXIT / QUIT         - Shut down node\n";
        } else {
            std::cout << "Unknown command: '" << cmd << "'. Type HELP for command list.\n";
        }

        std::cout << "lsm (" << config.node_id << ")> " << std::flush;
    }

    std::cout << "\nShutting down storage engine..." << std::endl;
    storage.Close();
    std::cout << "Node shut down cleanly." << std::endl;
    return 0;
}