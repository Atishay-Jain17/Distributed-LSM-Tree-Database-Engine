# Distributed Read Consistency Design (Week 6)

**Author**: Suhani  
**Module**: `reads/` (`lsmdb_reads`), headers in `include/lsmdb/reads/`, tests in `tests/reads/`  

---

## 1. Overview and Problem Statement

In a distributed key-value store coordinated by Raft, write operations are replicated through a quorum of nodes and committed in a globally agreed sequential order. However, serving read requests directly from local node storage introduces challenges:
1. **Stale Reads on Followers**: Replicas apply committed entries asynchronously. If a client reads from a follower node, it may read state that lags behind recently acknowledged writes.
2. **Network Partitions and Split-Brain Leaders**: A leader that has been partitioned from the cluster majority might continue serving client reads while a new leader has already been elected in the other partition. This violates **linearizability** (clients could read stale data or experience non-monotonic reads).

---

## 2. Consistency Levels (`ConsistencyLevel`)

The system provides three configurable consistency guarantees:

| Level | Serving Node | Quorum Verification | Guarantee |
|---|---|---|---|
| `kLinearizable` (Default) | Leader only | Required (heartbeat / lease verification) | **Strong Consistency / Linearizable**: Real-time read-your-writes and monotonic reads. |
| `kSequential` | Leader only | Local commit index verification | **Sequential Consistency**: Monotonically increasing across leader operations. |
| `kStaleTolerant` | Any replica | None | **Eventual / Monotonic per-node**: Low latency; may lag behind leader. |

---

## 3. Read Path Flow

### Linearizable Read Path

```text
 Client
   │
   │ 1. Get(key)
   ▼
[ Node ] (ConsistentStorageBackend / ReadRouter)
   │
   ├─► Is this node the leader?
   │     ├── NO  ──► Return STATUS_CODE_NOT_LEADER + leader_hint
   │     │           (Client automatically redirects to hinted leader)
   │     │
   │     └── YES ──► 2. Confirm Quorum (LeaderLeaseProvider)
   │                   ├── Quorum lost? ──► Return STATUS_CODE_UNAVAILABLE
   │                   │
   │                   └── Quorum OK   ──► 3. Read local committed state
   │                                           ├── Missing ──► STATUS_CODE_NOT_FOUND
   │                                           └── Found   ──► STATUS_CODE_OK (value)
```

1. **Client Initiation**: The client initiates a `Get(key)` using `ConsistentClusterClient`.
2. **Leader Check**:
   - If the receiving node is a follower, it immediately rejects the request with `STATUS_CODE_NOT_LEADER` and attaches the `leader_hint` (`host:port`).
   - The `ConsistentClusterClient` catches `NOT_LEADER`, extracts the leader hint, updates its known leader target, and re-submits the query directly to the leader.
3. **Quorum Confirmation**:
   - The leader confirms that it still holds an active lease or verifies quorum reachability.
   - If a network partition has isolated this node, `ConfirmQuorum()` returns `false`, and the read fails with `STATUS_CODE_UNAVAILABLE`, preventing stale reads.
4. **Local Execution**:
   - Once leadership is verified, the leader queries its local storage engine (`StorageBackend` / `LsmStore`).
   - Writes committed by Raft are applied prior to acknowledging the client, ensuring **Read-Your-Writes**.

---

## 4. Guarantees and Limitations

### Guarantees
- **Linearizability**: Under `kLinearizable`, once a write is acknowledged with `STATUS_CODE_OK`, all subsequent reads across the cluster observe that write or a later version.
- **Failover Transparency**: If the leader crashes, clients querying the old leader failover to newly elected leaders via `NOT_LEADER` hints or round-robin failover.
- **Tombstone Safety**: Deletes replicate through Raft and are respected immediately by reads (`STATUS_CODE_NOT_FOUND`).

### Limitations
- **Read Throughput**: Linearizable reads route through the leader, concentrating read load on the leader node.
- **Latency**: Leader lease confirmation incurs a network check or lease window verification. Applications tolerating stale data can use `kStaleTolerant` to read from local replicas at maximum throughput.
