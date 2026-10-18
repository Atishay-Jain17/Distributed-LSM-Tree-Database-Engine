# Distributed LSM-Tree Database Engine

This repository contains a C++20 coursework prototype and the imported `main` and `suhani-week5` implementation trees. The root CMake project builds Atishay's storage modules, Suhani's API/recovery/lifecycle modules, a persistent CLI node, and local demos. The optional distributed CLI node now includes a persistent Raft peer service and leader routing; its RPC/Raft build and live three-node deployment still require validation in a Linux environment with Protobuf/gRPC and Kubernetes tooling.

## Implemented Atishay modules

- **Week 1:** CMake foundation, status/configuration types, `StorageEngine` API, and a thread-safe in-memory `BasicStorageEngine` with PUT/GET/DELETE.
- **Week 2:** ordered SkipList and MemTable, iteration, entry count, and approximate memory accounting.
- **Week 3:** framed CRC32 WAL writer/reader, replay into a fresh MemTable, truncated-tail handling, and corruption detection.
- **Week 4:** sparse-index candidate selection, ordered serialization, and multi-table lookup. On Unix, the top-level CMake project also links this index into the imported `main` branch SSTable writer/reader; `VersionGet` uses indexed file reads across levels. The imported SSTable backend is POSIX-only and is omitted from the Windows root demo build.
- **Week 5–6:** the imported Atishay tree contains Raft state/log components. The Suhani CLI node also has a persistent peer protocol source path with elections, AppendEntries, quorum writes, ordered apply, leader redirects, and quorum read barriers. The gRPC implementation has not been compiled or run in this workstation environment.
- **Week 7:** dependency-free metrics collector and loopback HTTP `/metrics` endpoint on the demo server (default port 9090), with request counters and latency histograms. MemTable operations and WAL append latency are instrumented for the core library. SSTable/compaction metric setters exist, but the imported SSTable/compaction runtime is not yet wired to update them.
- **Week 8:** deterministic MemTable read/write benchmark, repeated three times, reporting throughput and per-operation P99 latency.

## Build and verify (Windows PowerShell)

```powershell
cmake -S . -B build/verify -G "MinGW Makefiles"
cmake --build build/verify
ctest --test-dir build/verify --output-on-failure
.\build\verify\lsm_storage_benchmark.exe
```

The storage benchmark prints CSV rows for three 20,000-operation runs. Results depend on the machine and should be reported with the configuration and date; they are not a comparison against disk-backed SSTables or another database.
The latest captured run is in [ATISHAY_WEEK8_BENCHMARK_RESULTS.csv](docs/ATISHAY_WEEK8_BENCHMARK_RESULTS.csv).

## Local terminal demo

Start the server in one terminal and the interactive client in another. The server also exposes scrapeable Prometheus text at `http://127.0.0.1:9090/metrics` (or the port selected with `--metrics-port`):

```powershell
.\build\verify\lsm_server.exe --port 50051 --metrics-port 9090 --data_dir .\data\mentor-demo
.\build\verify\lsm_client.exe --host 127.0.0.1 --port 50051
```

The TCP server defaults to port 50051 (metrics port 9090). Client commands: `PUT <key> <value>`, `GET <key>`, `DELETE <key>`, `INFO`, `HELP`, and `EXIT`. Server logs requests and in-memory entry/byte totals. Despite accepting a `data_dir` to keep the demo CLI shape, this baseline engine is an in-memory demo and does not persist records there.

For durable disk-backed storage and WAL crash recovery, use the standalone `lsmdb_local_cli`:

```powershell
.\build\verify\lsm-db\node\lsmdb_local_cli.exe --data-dir .\data\local-cli
```

In `lsmdb_local_cli`, the `INFO` command displays the exact storage engine telemetry:
`INFO sequence=<seq> flushed_up_to=<seq> rotations=<n> flushes=<n> compactions=<n> write_stalls=<n> wal_bytes=<bytes>`

Example Prometheus dashboard queries:

```promql
sum by (operation) (rate(storage_operations_total[1m]))
histogram_quantile(0.99, sum by (le, operation) (rate(storage_operation_latency_microseconds_bucket[5m])))
sum by (operation) (rate(storage_operation_errors_total[5m]))
```

## Web GUI Dashboard

A modern, minimal web dashboard is included in `lsm-db/gui/` to inspect cluster consensus state, topology, and execute live Key-Value operations with real-time feedback. It communicates directly with the real gRPC services and `kv_cli` backend via a thin adapter (zero extra dependencies).

### Features
- **Cluster Overview:** Live status cards for all cluster nodes showing Raft role (`LEADER` / `FOLLOWER`), term, commit index, last applied log index, and peer configuration.
- **Key-Value Operations:** Interactive PUT, GET, and DELETE execution against any target node, with automatic detection and visualization of follower-to-leader redirections.
- **Activity Feed:** Chronological event log tracking operation latencies, target vs handling node, and response statuses.
- **Cluster Lifecycle:** One-click controls to bootstrap or stop the 3-node local Raft cluster on ports 50051–50053.
- **Design:** Restrained, accessible warm stone palette with natural forest green, olive, teal, and amber accents. CLI compatibility is preserved 100%.

### Launching the Dashboard

**Windows:**
```powershell
python lsm-db/gui/server.py
# Or run the batch launcher:
.\lsm-db\gui\run_gui.bat
```

**Linux / WSL:**
```bash
python3 lsm-db/gui/server.py
# Or run the shell launcher:
./lsm-db/gui/run_gui.sh
```

Open your browser to: **`http://localhost:8080`**

To verify the GUI adapter and cluster operations programmatically:
```powershell
python lsm-db/gui/test_gui_adapter.py
```

## Imported branch trees

- `lsm-db-full-weeks1-6/lsm-db` contains the imported `main` branch project. Its POSIX storage library, including indexed SSTables and compaction code, is built by the top-level project on Unix systems.
- `lsm-db` contains the imported `suhani-week5` branch project.

The imported trees remain available in the repository and their modules are built where platform dependencies allow. The persistent CLI node uses the API/recovery/lifecycle path and Atishay's checksummed indexed SSTable store on Unix; Windows uses the portable reference store. The optional RPC server and CLI require Protobuf/gRPC tooling. The Helm chart configures three persistent node processes; deployment and consensus behavior remain unverified until the Linux/Kubernetes smoke and failure scenarios are run.

See [Atishay Weeks 1–8 delivery status](docs/ATISHAY_WEEKS_1_TO_8_REPORT.md) and [Suhani Weeks 1–8 delivery status](lsm-db/docs/SUHANI_WEEKS_1_TO_8_REPORT.md) for implementation details and remaining integration boundaries. Deployment instructions are in [the Kubernetes prototype guide](lsm-db/deploy/README.md).

