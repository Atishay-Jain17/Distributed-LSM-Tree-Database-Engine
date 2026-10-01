# Distributed LSM-Tree Database Engine

A distributed key-value storage engine combining an LSM-Tree based storage layer with strong consistency and fault tolerance across a three-node cluster.

## Overview

This project explores how **Database Management Systems, Operating Systems, and Distributed Systems** can work together in one storage engine.

The database uses an **LSM-Tree** architecture for write-heavy workloads. Recent writes are kept in an in-memory MemTable and recorded in a Write-Ahead Log (WAL) for recovery. When the MemTable becomes full, its contents are flushed to persistent SSTables, while background compaction keeps the on-disk representation manageable.

At the distributed level, **Raft** coordinates the three database nodes. Writes are routed through the leader and are committed after a quorum acknowledges the replicated log entry. This provides a consistent view of data while allowing the cluster to tolerate node failures.

At the operating-system level, the project investigates **Linux io_uring and O_DIRECT** for asynchronous and direct disk I/O. Their performance is evaluated against traditional POSIX I/O rather than assumed to be faster.

## Key Features

- Distributed key-value storage across a three-node cluster
- LSM-Tree based storage engine
- In-memory SkipList MemTable
- Multi-Version Concurrency Control (MVCC)
- Append-only Write-Ahead Log for crash recovery
- SSTable-based persistent storage
- Sparse indexes and Bloom filters for SSTable reads
- Background leveled compaction
- Raft leader election and log replication
- Quorum-based commits
- gRPC communication using Protocol Buffers
- Linux asynchronous I/O using liburing / io_uring
- O_DIRECT for direct disk I/O where appropriate
- Kubernetes-based deployment
- Terraform infrastructure provisioning
- Prometheus and Grafana monitoring
- Chaos Mesh and Jepsen-based failure testing
- GitHub Actions for automation

## High-Level Architecture

```text
                         Client / Application
                                  |
                                  v
                         +------------------+
                         |   Raft Leader    |
                         +------------------+
                           /              \
                          /                \
                         v                  v
                +---------------+    +---------------+
                |    Node 2     |    |    Node 3     |
                |   Follower    |    |   Follower    |
                +---------------+    +---------------+
                         \                  /
                          \                /
                           +------Raft-----+

                    Storage Engine on Each Node
                    ===========================

                         Incoming Write
                               |
                               v
                         +-----------+
                         | MemTable  |
                         |  (RAM)    |
                         +-----------+
                           |       |
                           |       v
                           |   +--------+
                           |   |  WAL   |
                           |   +--------+
                           |       |
                           |    Disk I/O
                           |
                           v
                       +---------+
                       | SSTable |
                       +---------+
                           |
                           v
                    Background Compaction

              SSTable Reads:
              Bloom Filter -> Sparse Index -> Disk
```

## Storage Design

Each node contains an LSM-Tree based storage engine.

### MemTable

Incoming writes are assigned MVCC timestamps and stored in an in-memory SkipList protected by read-write locks.

Because recent data remains in memory, the system does not need to immediately perform a disk operation for every logical write.

### Write-Ahead Log

Before a write is acknowledged by the MemTable, its payload is appended to the WAL. The WAL provides a persistent record that can be used to recover operations after a crash.

The WAL append path is designed to use `io_uring` and direct I/O as part of the project's OS-level performance investigation.

### SSTables

When the MemTable reaches its configured limit, its contents are flushed to disk as immutable Sorted String Tables (SSTables).

SSTables provide the persistent representation of the database.

### Compaction

A background worker pool performs leveled compaction by merging SSTables. This keeps the number of files manageable and reduces unnecessary write amplification.

### Reads

A read first considers the most recent in-memory state. If the required value is not present there, the SSTable layer can use Bloom filters and sparse indexes to avoid unnecessary disk searches before performing the required disk read.

## Distributed Consensus

The database uses **Raft over gRPC** to coordinate the three nodes.

The cluster elects one node as the leader. Client writes are routed to the leader, which adds the operation to its replicated log. The operation becomes committed after a quorum of nodes acknowledges it.

If the leader fails, Raft's election mechanism allows another node to become leader.

The goal is to maintain a consistent, up-to-date state across the cluster while tolerating individual node failures.

## Operating-System I/O

A major part of the project is investigating the interaction between the database storage engine and Linux storage I/O.

### io_uring

`io_uring` provides an asynchronous I/O interface using kernel submission and completion queues. The project uses it for the WAL append path so application threads do not have to rely entirely on traditional blocking I/O.

### O_DIRECT

`O_DIRECT` is investigated for direct disk I/O, reducing reliance on the operating system's normal page-cache path.

These mechanisms are **not treated as guaranteed performance improvements**. The project will benchmark asynchronous I/O against traditional POSIX I/O and measure metrics such as throughput and P99 latency.

## Technology Stack

| Layer | Technologies |
|---|---|
| Core Languages | C++20 or Rust |
| Storage & OS | Linux 5.1+, liburing / io_uring, O_DIRECT |
| Network & RPC | gRPC, Protocol Buffers |
| Infrastructure | Terraform, Kubernetes, Helm |
| Observability | Prometheus, Grafana |
| Chaos & Testing | Chaos Mesh, Jepsen, GitHub Actions |

## Deployment

The system is intended to run as a three-node cluster using Kubernetes StatefulSets.

Terraform is used for infrastructure provisioning, while Kubernetes provides stable node identity, peer discovery, and storage attachment. Prometheus collects system and database metrics, which are visualized through Grafana.

## Testing and Benchmarking

The project includes both correctness testing and performance evaluation.

Failure testing will simulate scenarios such as:

- Database node crashes
- Network partitions
- Packet loss
- Disk degradation

Performance testing will measure:

- Write throughput
- Read performance
- P99 latency
- I/O behavior
- Raft election latency
- Recovery behavior

A key experiment will compare **io_uring-based I/O with traditional POSIX I/O** under high-throughput workloads.

## Project Goals

The final prototype aims to demonstrate:

1. A functional distributed key-value database.
2. Efficient LSM-Tree based storage and background compaction.
3. Consistent replicated operations using Raft.
4. Crash recovery using the Write-Ahead Log.
5. Practical integration of OS-level asynchronous/direct I/O.
6. Measurable performance results through benchmarking.
7. Resilience under simulated hardware and network failures.
8. Monitoring of both storage-level and cluster-level behavior.

## Project Structure

The exact source-tree structure will be finalized during implementation. The expected components are:

```text
.
├── storage/          # MemTable, WAL, SSTables, compaction
├── consensus/        # Raft and replicated log
├── rpc/              # gRPC and Protobuf interfaces
├── io/               # io_uring / direct I/O layer
├── tests/             # Correctness and failure tests
├── benchmarks/        # Performance benchmarks
├── deploy/            # Kubernetes / Helm configuration
├── terraform/         # Infrastructure provisioning
└── monitoring/        # Prometheus / Grafana configuration
```

## Development Plan

The project is planned as an eight-week implementation:

- **Week 1:** Foundation, infrastructure, Kubernetes and gRPC setup
- **Week 2:** Concurrent MemTable and MVCC
- **Week 3:** WAL, liburing and O_DIRECT
- **Week 4:** Compaction, sparse indexes and Bloom filters
- **Week 5:** Raft leader election
- **Week 6:** Log replication and quorum commits
- **Week 7:** Monitoring and chaos testing
- **Week 8:** Stabilization, benchmarking and documentation

## Project Outcome

The intended outcome is a working distributed LSM-Tree key-value storage engine with strong consistency, crash recovery, OS-level I/O experimentation, automated failure testing, and measurable benchmark results.

The project is primarily an engineering and experimental system intended to demonstrate the interaction between storage engines, operating-system I/O, and distributed consensus.

## Quick Start & Build

For comprehensive details on the storage API, configuration, and team integration points, see [WEEK1_STORAGE_HANDOFF.md](docs/WEEK1_STORAGE_HANDOFF.md).

### Building with CMake
```bash
# Configure
cmake -B build

# Build (libraries, node daemon, and tests)
cmake --build build

# Run unit tests
./build/lsm_unit_tests

# Run smoke test
./build/lsm_smoke_test
```
