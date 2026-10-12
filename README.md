# Distributed LSM-Tree Database Engine

This repository contains a C++20 coursework prototype and the imported `main` and `suhani-week5` implementation trees. The root CMake project builds the Atishay-owned modules and the localhost CLI demo. It does **not** yet run a complete replicated, persistent LSM database.

## Implemented Atishay modules

- **Week 1:** CMake foundation, status/configuration types, `StorageEngine` API, and a thread-safe in-memory `BasicStorageEngine` with PUT/GET/DELETE.
- **Week 2:** ordered SkipList and MemTable, iteration, entry count, and approximate memory accounting.
- **Week 3:** framed CRC32 WAL writer/reader, replay into a fresh MemTable, truncated-tail handling, and corruption detection.
- **Week 4:** sparse-index candidate selection, ordered serialization, and multi-table lookup. On Unix, the top-level CMake project also links this index into the imported `main` branch SSTable writer/reader; `VersionGet` uses indexed file reads across levels. The imported SSTable backend is POSIX-only and is omitted from the Windows root demo build.
- **Week 5:** Raft term/vote state, persistent term/vote file, request-vote rules, simulated three-node election, and randomized timeout generation. This is state-machine logic, not a networked election service.
- **Week 6:** in-memory Raft log, follower prefix consistency/conflict handling, conflict hints, and ordered application of committed commands to a storage API. No leader replication loop or quorum transport is wired.
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

Client commands: `PUT <key> <value>`, `GET <key>`, `DELETE <key>`, `INFO`, `HELP`, and `EXIT`. Server logs requests and in-memory entry/byte totals. Despite accepting a `data_dir` to keep the demo CLI shape, this baseline engine does not persist records there.

Example Prometheus dashboard queries:

```promql
sum by (operation) (rate(storage_operations_total[1m]))
histogram_quantile(0.99, sum by (le, operation) (rate(storage_operation_latency_microseconds_bucket[5m])))
sum by (operation) (rate(storage_operation_errors_total[5m]))
```

## Imported branch trees

- `lsm-db-full-weeks1-6/lsm-db` contains the imported `main` branch project. Its POSIX storage library, including indexed SSTables and compaction code, is built by the top-level project on Unix systems.
- `lsm-db` contains the imported `suhani-week5` branch project.

They remain separate projects with overlapping module names. The Windows root demo does not combine their runtime paths. Their full project builds require the dependencies declared by their own project, including Protobuf/gRPC tooling. The Linux SSTable library is included separately for indexed file storage and multi-file reads, but RPC, recovery orchestration, and a three-node cluster are not wired into the root demo.

See [Atishay Weeks 1–8 delivery status](docs/ATISHAY_WEEKS_1_TO_8_REPORT.md) for implementation details and remaining integration boundaries.