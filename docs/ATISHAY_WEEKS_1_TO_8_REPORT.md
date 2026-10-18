# Atishay — Weeks 1–8 Delivery Status

This report documents Atishay's assigned work and team integration based on the available repository documents (the team execution PDF `Distributed_LSM_Tree_8_Week_Work_Report_FINAL(1).pdf` is absent from this workspace; requirements are derived from existing project proposals, architecture docs, and code). It distinguishes implemented code from components that still need team integration.

## Week-by-week

### Week 1 — Foundation and baseline storage

Implemented a C++20/CMake foundation, status and configuration types, the `StorageEngine` interface, and a thread-safe in-memory `BasicStorageEngine`. PUT, GET, overwrite, DELETE, missing-key errors, and lifecycle behavior have unit and smoke coverage. The baseline engine is memory-only; its `data_dir` creates a directory but does not make records persistent.

### Week 2 — SkipList and MemTable

Implemented an ordered probabilistic SkipList with lookup, update, deletion, seek, and iteration. `MemTable` wraps it and reports entry count and approximate memory usage. A deterministic microbenchmark is included.

### Week 3 — WAL and replay

Implemented framed WAL records with CRC32, append, read, and replay into a fresh MemTable. Truncated final writes are reported and ignored; checksum corruption, malformed records, and non-increasing sequence numbers fail as corruption. `sync_on_append` flushes the stream and calls `fsync` on POSIX or `FlushFileBuffers` on Windows; `Sync()` and `Close()` also request a durable flush.

### Week 4 — Sparse index and SSTable lookup

Implemented ordered sparse-index serialization, binary-search candidate selection, and multi-table lookup abstraction. On Unix, top-level CMake links the index into the imported `main` branch SSTable writer/reader: the writer samples record-boundary keys every 64 records and persists an aligned index section; `SSTableReader::Get` reads the candidate block; `VersionGet` uses that lookup across files/levels. Legacy index-free SSTables retain the sequential-read fallback. The POSIX SSTable path is excluded from the Windows build, so this integration still needs to be built and tested on Linux.

### Week 5 — Raft state and elections

The imported Atishay tree implements follower/candidate/leader state, term and vote persistence, RequestVote eligibility/log freshness checks, randomized timeout generation, and an in-process three-node election simulation. State-file replacement is atomic on supported Windows/POSIX paths and malformed/truncated files are rejected. The combined Suhani CLI path also now includes a separate persistent gRPC Raft implementation; its network path is not yet verified in this environment.

### Week 6 — Replicated log

The imported Atishay tree implements an in-memory Raft log with previous-index/term checks, conflict hints, conflicting uncommitted suffix replacement, and ordered application through `StorageEngine`. The combined Suhani CLI source path adds leader replication, retries, majority commit, persisted log state, current-term quorum reads, and a three-node integration test. Protobuf/gRPC are absent from this workstation, so that implementation and test have not been compiled or run.

### Week 7 — Storage metrics

Added a thread-safe metrics collector with Prometheus text export, latency histogram buckets, request counters, WAL append latency, MemTable byte gauge, SSTable/compaction metrics wired through optional VersionSet/CompactionManager collectors, operation error counters, and a loopback HTTP `/metrics` endpoint in the local demo server. The demo endpoint was exercised with PUT/GET/DELETE and returned HTTP 200 with counters. The root demo uses `BasicStorageEngine`, so its WAL/MemTable/SSTable/compaction gauges remain zero; the root demo does not construct the imported VersionSet/CompactionManager with its collector, so those demo gauges remain zero. Example PromQL queries and error counters are included. Suhani's separate Kubernetes/Chaos Mesh failure-injection runner is documented in the deployment report and has not been executed in this workstation environment; no Grafana dashboard configuration is included.

### Week 8 — Stabilization and benchmark

Added a deterministic 20,000-operation MemTable read/write workload repeated three times, reporting throughput and per-operation P99 latency as CSV. In the latest run on Windows 11 / MinGW GCC 16.1.0, median throughput was about 1.02M writes/s (P99 2 µs) and 0.91M reads/s (P99 1 µs). This measures the local in-memory storage engine only, not full-application, disk/SSTable, or distributed Raft performance. P99 values are quantized by the microsecond timer and should be treated as rough local measurements, not portable performance claims.

## Verification on this Windows workspace

- Root CMake configured with MinGW GCC 16.1.0; all root targets built in `build/verify`.
- Root CTest: 6/6 groups passed in the current checkout (UnitTests [56 tests], SmokeTest, BloomFilterSmoke, CliPersistenceSmoke, CliBinarySmoke, NodeStorageSmoke). This does not compile the optional RPC/Raft target. A separate MinGW compatibility harness compiled the imported POSIX SSTable/compaction library and passed 3 focused index/metrics tests; this is not a native Linux run.
- Local TCP demo: PUT, GET, DELETE returned successful responses.
- Metrics HTTP endpoint: returned HTTP 200 and operation counter/latency series.
- The POSIX SSTable path is excluded from the normal Windows root build. Its source and indexed reads were exercised through a temporary compatibility harness; native Linux `pread`/`fsync` behavior remains unverified because WSL is not installed.

## Scope boundary / remaining integration

The local root terminal demo remains a single-process, in-memory service. The persistent Suhani CLI node uses WAL recovery and reference-format SSTables rather than the imported indexed/Bloom SSTable backend. Network Raft, imported SSTable integration, io_uring, and live metrics/deployment/failure scenarios still need toolchain-enabled integration and validation. The current Windows CTest pass is not proof of those paths.
