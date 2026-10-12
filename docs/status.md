# Project Gap Report & Component Status

**Plan**: Distributed LSM-Tree Database Engine (8-Week Plan: 4 Sep – 29 Oct 2026)  
**Reconciliation Date**: 13 Oct 2026  
**Audited Against**: `origin/main` (HEAD: `c7fa11d`) and `suhani-integrated`  

---

## 1. Team Responsibilities Summary

- **Atishay**: Core storage, WAL, sparse index, Raft state & log, storage benchmarks
- **Sarthak**: MVCC, io_uring / O_DIRECT, Bloom filters, Raft heartbeat / quorum, I/O benchmarks
- **Vinayak**: gRPC / RPC, SSTables, compaction, peer discovery, distributed writes, reliability tests
- **Suhani**: Storage API & integration, recovery, lifecycle, Raft-to-storage boundary, distributed reads, monitoring / chaos, deployment / docs

---

## 2. Weekly Status Breakdown (Done / Stand-in / Missing)

### Week 1 (4 – 10 Sep 2026): Foundation & Infrastructure
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Suhani** | 3-node K8s StatefulSet, headless service, Helm chart, smoke tests, lifecycle tests | **Done** | Implemented in `deploy/` (`helm/lsm-db`, `smoke-test.sh`, `lifecycle-test.sh`) |
| **Vinayak** | gRPC definitions (`kv.proto`, `node.proto`), `KvServiceImpl`, `NodeServiceImpl`, `NodeServer`, RPC tests | **Done** | Merged in `origin/main` (`proto/`, `rpc/`, `tests/rpc/`) |
| **Atishay** | Core storage foundation, storage configuration options | **Missing** (on `origin/main`) | Unmerged on `origin/atishay` in root `src/`. Stand-in provided by Suhani (`MemTableBackend`) |
| **Sarthak** | Foundation I/O & project setup | **Missing** | No commits on `origin/main` |

### Week 2 (11 – 17 Sep 2026): Concurrent MemTable & MVCC
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Sarthak** | Concurrent SkipList MemTable, MVCC timestamps/versions | **Missing** (on `origin/main`) | Not landed |
| **Vinayak** | Storage backend abstraction (`StorageBackend`, `InMemoryBackend`), initial SSTable structures | **Done** | Merged in `origin/main` (`rpc/storage_backend.h`, `storage/memtable.h`) |
| **Atishay** | MemTable / SkipList implementation | **Missing** (on `origin/main`) | Exists only in unmerged branch |
| **Suhani** | Single-node storage API over MemTable interface | **Done** | Implemented in `api/` (`memtable.h`, `memtable_backend.h`, `reference_memtable.h`, tests) |

### Week 3 (18 – 24 Sep 2026): WAL, io_uring & Crash Recovery
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Atishay** | Append-only WAL, WAL recovery logic | **Stand-in** | Real code not on `origin/main`. Suhani's `ReferenceWal` provides functional stand-in |
| **Sarthak** | liburing / io_uring integration, O_DIRECT disk I/O | **Missing** | Not landed on `origin/main` |
| **Vinayak** | SSTable reader/writer, block format, CRC32 verification | **Done** | Merged in `origin/main` (`storage/sstable_writer.h`, `sstable_reader.h`, `crc32.h`) |
| **Suhani** | Startup recovery flow & WAL replay | **Done** | Implemented in `recovery/` (`recovery.h`, `wal_reader.h`, replay tests) |

### Week 4 (25 Sep – 1 Oct 2026): Compaction & Filtering
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Vinayak** | Background leveled compaction, compaction manager, compaction picker, version set | **Done** | Merged in `origin/main` (`storage/compaction_manager.h`, `version_set.h`) |
| **Sarthak** | Bloom filters for SSTable fast-path lookup | **Missing** | Not landed on `origin/main` |
| **Atishay** | Sparse index for SSTable block indexing | **Missing** (on `origin/main`) | Unmerged on `origin/atishay` |
| **Suhani** | MemTable rotation, flush worker, compaction coordination | **Done** | Implemented in `lifecycle/` (`lsm_store.h`, `reference_table_store.h`, tests) |

### Week 5 (2 – 8 Oct 2026): Raft Leader Election & Node Discovery
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Atishay** | Raft state machine, leader election, log storage | **Stand-in** | Stand-in via `tests/support/fake_raft.h` & `consensus_boundary/reference_consensus.h` |
| **Sarthak** | Raft heartbeats, randomized election timeouts | **Missing** | Not landed on `origin/main` |
| **Vinayak** | Kubernetes peer discovery, peer manager | **Done** | Merged in `origin/main` (`consensus/peer_discovery.h`, `peer_manager.h`) |
| **Suhani** | Replicated command format, validation, Raft-to-storage boundary | **Done** | Implemented in `consensus_boundary/` (`boundary.h`, `command.h`, `storage_state_machine.h`) |

### Week 6 (9 – 15 Oct 2026): Distributed Writes, Reads & Replication
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Vinayak** | Distributed write path (`ReplicatedWriteHandler`, `OrderedApplier`, `ClusterClient`, failover) | **Done** | Merged in `origin/main` (all 216 ctest suite tests passing) |
| **Atishay** | Raft log replication, log matching | **Stand-in** | Simulated via `fake_raft.h` |
| **Sarthak** | Quorum commit logic, replication heartbeats | **Stand-in** | Simulated via `fake_raft.h` quorum checks |
| **Suhani** | Distributed reads, leader read routing, linearizable consistency checks | **In Progress** | Current target milestone (Week 6) |

### Week 7 (16 – 22 Oct 2026): Observability & Chaos Testing
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Atishay** | Storage metrics (WAL latency, MemTable size, SSTable count/size) | **Missing** | Expected metrics listed in `docs/integration-requests.md` |
| **Sarthak** | Raft metrics (term, role, election counts, replication lag) | **Missing** | Expected metrics listed in `docs/integration-requests.md` |
| **Vinayak** | Reliability tests, partition recovery tests | **Partial** | Initial cluster integration tests in `cluster_integration_test.cpp` |
| **Suhani** | Chaos Mesh manifests (PodChaos, NetworkChaos), test runner, Jepsen linearizability checker | **In Progress** | Current target milestone (Week 7) |

### Week 8 (23 – 29 Oct 2026): Stabilization, Benchmarking & Delivery
| Member | Expected Component | Status in `origin/main` / branch | Notes |
|---|---|---|---|
| **Atishay** | Storage benchmarks (throughput, P99 latency) | **Missing** | Blocked on real storage landing |
| **Sarthak** | io_uring vs POSIX benchmarks | **Missing** | Blocked on io_uring landing |
| **Vinayak** | Distributed write throughput benchmarks | **Partial** | Initial harness in `benchmarks/memtable_contention.cpp` |
| **Suhani** | Deployment hardening, evidence tree consolidation, docs audit, release packaging | **In Progress** | Current target milestone (Week 8) |

---

## 3. Stand-In Decision Matrix for Weeks 6–8

To make progress without violating Rule 3 (do not edit teammates' modules) or Rule 4 (do not fabricate):
1. **Consensus / Raft Proposer**:
   - Utilize Vinayak's `lsmdb::testing_support::FakeRaftCluster` (`tests/support/fake_raft.h`) and Suhani's `ReferenceConsensus` as the standard stand-ins for multi-node and single-node consensus.
   - All Week 6 read routing and consistency tests run against this explicit, fully tested interface.
2. **Wire Command Unification**:
   - Standardize on Protobuf `v1::ReplicatedCommand` for the Raft log wire format to maintain 100% compatibility with Vinayak's merged write-path.
   - Suhani's `consensus_boundary` provides adapters and strict validation matching `rpc/validation.h`.
3. **Metrics & Monitoring**:
   - Provide deploy-side Helm configs (Prometheus ServiceMonitor, Grafana dashboards) ready to ingest metrics, and document expected metric names in `docs/integration-requests.md`.
4. **Chaos Testing**:
   - Produce real Chaos Mesh manifests in `deploy/chaos/`. For local testing, run process-level fault injection (SIGKILL, SIGSTOP/CONT) against local multi-node clusters and clearly mark environment execution status in `evidence/chaos/`.
