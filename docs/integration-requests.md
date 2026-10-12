# Integration Requests for Teammates

**Author**: Suhani  
**Last Updated**: 13 Oct 2026  
**Purpose**: Document proposed changes to teammate-owned modules per Rule 3.

---

## 1. Request for Vinayak: Add `request_id` to `ReplicatedCommand`

- **Target File**: `lsm-db-full-weeks1-6/lsm-db/proto/lsmdb/v1/command.proto`
- **Proposed Change**:
  ```protobuf
  message ReplicatedCommand {
    CommandType type = 1;
    bytes key = 2;
    bytes value = 3;
    string request_id = 4;  // Optional client request ID for deduplication and tracing
  }
  ```
- **Rationale**:
  In distributed consensus, clients retry proposals upon transient timeouts. Without a unique `request_id` in the replicated command payload, state machines cannot deduplicate retried client proposals. Adding field 4 is 100% backward-compatible in Protobuf and unifies Suhani's consensus boundary validation with Vinayak's wire format.
- **Adapter on Suhani's side**:
  Suhani's `consensus_boundary` provides an adapter converting `consensus_boundary::Command` to and from `v1::ReplicatedCommand`, serializing/deserializing transparently.

---

## 2. Request for Vinayak: Read Consistency Routing Hook in `KvServiceImpl`

- **Target File**: `lsm-db-full-weeks1-6/lsm-db/rpc/include/lsmdb/rpc/kv_service_impl.h`
- **Proposed Change**:
  Allow registering an optional read handler:
  ```cpp
  using ReadHandler = std::function<v1::Status(const std::string& key, std::string* out_value)>;
  ```
  When set, `KvServiceImpl::Get` delegates to the handler, enabling leader lease verification and follower redirect with leader hint.
- **Adapter on Suhani's side**:
  Suhani implements the linearizable read router and adapter in `reads/` (`ConsistentReadHandler` and `ConsistentStorageBackend`).

---

## 3. Request for Atishay & Sarthak: Expected Prometheus Metrics Endpoints

- **Target Modules**: `rpc/node_server.cpp`, `rpc/node_main.cpp`
- **Expected Metric Names**:
  - Storage:
    - `lsmdb_memtable_size_bytes` (Gauge)
    - `lsmdb_sstable_count` (Gauge)
    - `lsmdb_wal_append_latency_microseconds` (Histogram)
    - `lsmdb_compaction_duration_seconds` (Histogram)
  - Consensus:
    - `lsmdb_raft_term` (Gauge)
    - `lsmdb_raft_is_leader` (Gauge, 0 or 1)
    - `lsmdb_raft_commit_index` (Counter)
    - `lsmdb_raft_applied_index` (Counter)
- **Deployment Wiring**:
  Deploy manifests in `deploy/monitoring/` (Prometheus ServiceMonitor and Grafana dashboard templates) are prepared to scrape port 50051 once metric endpoints are bound.
