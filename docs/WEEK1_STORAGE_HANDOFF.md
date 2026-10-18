# Week 1 Storage Foundation & Handoff Specification

**Author**: Atishay (Team Lead / Storage Engine Core)  
**Milestone**: Week 1 — Foundation, Infrastructure & Interfaces  
**Status**: Completed and Verified  

---

## 1. Overview

In Week 1, the repository and build foundation for the Distributed LSM-Tree Database Engine was established. As part of Atishay's task allocation, the core storage engine abstract interface, the basic single-node storage implementation, the unified configuration layer, the unified status/error system, unit tests, and smoke tests were implemented and verified.

---

## 2. Directory & Module Structure

```text
.
├── CMakeLists.txt                    # Root build configuration (C++20, warnings, targets, CTest)
├── docs/
│   └── WEEK1_STORAGE_HANDOFF.md      # This handoff and interface specification
├── include/
│   ├── common/
│   │   ├── config.h                  # Node, Storage, and Network configuration structs
│   │   └── status.h                  # Status and StatusCode class (Google/RocksDB style)
│   └── storage/
│       ├── basic_storage_engine.h    # Week 1 in-memory baseline storage engine
│       └── storage_engine.h          # Abstract storage engine interface contract
├── src/
│   ├── common/
│   │   ├── config.cpp                # Config file parsing, CLI flags, validation
│   │   └── status.cpp                # Status string formatting and utilities
│   ├── storage/
│   │   └── basic_storage_engine.cpp  # BasicStorageEngine implementation with size tracking & concurrency
│   └── main.cpp                      # Node daemon executable entry point with interactive CLI
└── tests/
    ├── test_framework.h              # Lightweight, zero-dependency unit test framework
    ├── test_main.cpp                 # Unit test suite runner
    ├── test_storage.cpp              # Storage correctness tests (PUT, GET, DELETE, overwrites, missing keys)
    ├── test_config.cpp               # Config parsing, validation, and CLI tests
    └── smoke_test.cpp                # High-volume single-node storage smoke test
```

---

## 3. Local Build Instructions

### Prerequisites
* **C++ Compiler**: GCC 11+ or Clang 13+ (or MinGW-w64 GCC 13+ on Windows) with C++20 support.
* **CMake**: Version 3.20 or newer.
* **Build tool**: `make`, `ninja`, or `mingw32-make`.

### Build on Windows (MinGW)
```powershell
# Configure build directory
cmake -B build -G "MinGW Makefiles"

# Compile all targets (libraries, daemon, tests)
cmake --build build
```

### Build on Linux
```bash
# Configure build directory
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile all targets
cmake --build build -j$(nproc)
```

---

## 4. Running the Node & Tests

### Running the Node Daemon
```powershell
# Run with default settings
.\build\lsm_node.exe

# Run with custom parameters
.\build\lsm_node.exe --node_id node-1 --client_port 50051 --peer_port 50052 --data_dir ./data/node1

# Run with a configuration file
.\build\lsm_node.exe --config config/node1.conf
```

#### Interactive CLI Commands
Inside the `lsm_node` shell:
* `PUT <key> <value>` — Store key-value pair.
* `GET <key>` — Retrieve value for key.
* `DELETE <key>` — Delete key from engine.
* `CONTAINS <key>` — Check key presence.
* `INFO` — Show live node and storage statistics.
* `HELP` — Display available commands.
* `EXIT` or `QUIT` — Gracefully shut down node and storage engine.

### Running Unit Tests
```powershell
.\build\lsm_unit_tests.exe
```

### Running the Storage Smoke Test
```powershell
.\build\lsm_smoke_test.exe
```

### Running via CTest
```powershell
ctest --test-dir build --output-on-failure
```

---

## 5. Storage Engine Interface Contract (`lsm::StorageEngine`)

The abstract class `lsm::StorageEngine` (`include/storage/storage_engine.h`) serves as the long-term contract for the database engine. In subsequent weeks, `BasicStorageEngine` will be replaced with `LSMStorageEngine` (MemTable, WAL, SSTable) without altering downstream callers:

```cpp
namespace lsm {

class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    virtual Status Open() = 0;
    virtual Status Close() = 0;

    virtual Status Put(const std::string& key, const std::string& value) = 0;
    virtual Status Get(const std::string& key, std::string* value) const = 0;
    virtual Status Delete(const std::string& key) = 0;

    virtual bool Contains(const std::string& key) const = 0;
    virtual size_t EntryCount() const = 0;
    virtual size_t ApproximateSize() const = 0;
    virtual void Clear() = 0;
};

} // namespace lsm
```

### Return Status Codes (`lsm::StatusCode`)
* `StatusCode::Ok` (0): Operation completed successfully.
* `StatusCode::NotFound` (1): The requested key was not present in storage.
* `StatusCode::AlreadyExists` (2): Key or entity already exists.
* `StatusCode::InvalidArgument` (3): Invalid argument provided (e.g. empty key, null pointer).
* `StatusCode::IOError` (4): Underlying filesystem or storage I/O error.
* `StatusCode::Corruption` (5): Data corruption detected.
* `StatusCode::NotSupported` (6): Feature or operation not supported.
* `StatusCode::InternalError` (7): Unrecoverable internal error.

---

## 6. Configuration Layer

`NodeConfig` (`include/common/config.h`) unifies settings across subsystems:

```cpp
struct StorageConfig {
    std::string data_dir = "./data";
    size_t max_memtable_size = 64 * 1024 * 1024; // 64 MB
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
};
```

### Configuration File Format
```ini
# Node Identity
node.id = node-1

# Storage
storage.data_dir = ./data/node1
storage.max_memtable_size = 67108864
storage.sync_wal = true

# Network
network.client_port = 50051
network.peer_port = 50052
network.metrics_port = 9090
```

---

## 7. Team Integration Points (Handoff)

* **To Vinayak (gRPC & RPC Service Skeleton)**:
  * You can link against `storage_lib` and `common_lib`.
  * Instantiate `std::shared_ptr<lsm::StorageEngine>` (or `std::unique_ptr`) and route incoming gRPC `Put`, `Get`, `Delete` requests straight into `storage->Put(req.key(), req.value())`, `storage->Get(req.key(), &val)`, and `storage->Delete(req.key())`.
  * Map `lsm::StatusCode` directly to gRPC status codes (`StatusCode::NotFound` -> `grpc::StatusCode::NOT_FOUND`, `StatusCode::InvalidArgument` -> `grpc::StatusCode::INVALID_ARGUMENT`, etc.).

* **To Suhani (Kubernetes & Deployment)**:
  * Node identity and ports can be injected via CLI args (`--node_id $(POD_NAME)`, `--client_port 50051`, `--peer_port 50052`, `--data_dir /var/lib/lsm/data`).
  * The node exits cleanly on interactive `EXIT`/`QUIT` commands or standard input EOF; it does not currently install OS signal handlers (e.g., SIGINT/SIGTERM).

* **To Sarthak (I/O Abstraction & liburing)**:
  * The `StorageConfig` structure contains `data_dir` and `sync_wal` flags. Future I/O engines (POSIX baseline, io_uring) can be plugged directly behind the storage and WAL append boundary.

---

## 8. Week 1 Known Limitations

1. **In-Memory Storage Only**: `BasicStorageEngine` stores all records strictly in memory. While `Open()` creates the configured data directory on the filesystem if it does not already exist, no data records are persisted there, and all data is lost when the process exits. Persistent storage via Write-Ahead Log (WAL) and SSTables will be introduced in Week 3.
2. **Single-Node**: Clustering and replication logic will be added in Weeks 5–6 (Raft consensus).
3. **No Range Scans / Iterators**: Ordered key iterations will be added in Week 2 with the SkipList MemTable.
