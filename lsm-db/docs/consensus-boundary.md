# Consensus ↔ RPC ↔ storage boundary (Week 5)

Code: `consensus/` (`Command`, `StateMachine`, `Proposer`, `StorageStateMachine`, `RaftNode`,
`ReferenceConsensus`, `ReplicatedBackend`), tests: `tests/consensus/`. `RaftNode` implements the
networked peer service and proposes/apply integration; `ReferenceConsensus` remains the single-node
test stand-in.

## 1. Who talks to whom

```
 client ──gRPC──► KvServiceImpl            (rpc/, validates the request, Vinayak)
                      │ Put / Get / Delete
                      ▼
              ReplicatedBackend             (consensus/)  implements rpc::StorageBackend
        write │                    │ read (Get)
              ▼                    ▼
   Proposer::Propose(Command)   local storage (MemTableBackend / LsmStore)
              │  Raft: append, replicate, wait for majority = COMMIT
              ▼
   StateMachine::Apply(index, bytes)  ◄── called by Raft for every committed entry, every node
              │ decode + validate
              ▼
        StorageStateMachine ──Put/Delete──► storage
```

* **RPC** knows only `StorageBackend`. `node_main.cpp` supplies `ReplicatedBackend`; `RaftService`
  hosts `RequestVote` and `AppendEntries` beside the unchanged KV API.
* **Consensus** knows only `Command` bytes. It never looks at keys or values.
* **Storage** knows only `Put/Delete`. It never sees a log index or a term.

## 2. The replicated command

`Command { type: Put | Delete, key, value, request_id }`. Encoded (`Encode`/`Decode`) as:

| offset | size | field |
|---|---|---|
| 0 | 1 | format version (= 1) |
| 1 | 1 | type (1 = put, 2 = delete) |
| 2 | 4 | key length (little-endian u32) |
| 6 | 4 | value length |
| 10 | 4 | request_id length |
| 14 | … | key, then value, then request_id |

* The Raft log stores these bytes as the entry payload. `AppendEntries` should carry them in a
  `bytes` field (the largest possible command is `kMaxEncodedCommandBytes` ≈ 1 MiB + 1.2 KiB, under
  the 2 MiB gRPC cap; a compile-time check in the gRPC test enforces it).
* A delete is its own command (a tombstone is written by storage), with an empty value.
* The layout is pinned by a test. To change it, add a new version number and keep decoding version 1.
* `Decode` is strict: truncated input, trailing bytes, unknown version/type and lengths that do not
  match the buffer are all errors, and no input can make it read out of bounds.

## 3. Validation (before storage)

`Validate(command)` enforces: known type, key 1..1024 bytes, value ≤ 1 MiB, delete has no value,
request_id ≤ 128 bytes. It runs twice:

1. in `Propose`, so an invalid command is refused to the caller (`kInvalid`) and **never enters the log**;
2. in `StorageStateMachine::Apply`, so a bad entry that somehow reached the log (bug, old version,
   corruption) is **never written to storage**. Rejection is deterministic, so every replica makes
   the same choice; the entry is still counted as consumed (`LastApplied` advances) so replicas stay in step.

The limits equal the RPC layer's (`rpc/validation.h`); the gRPC test fails if they drift apart.

## 4. Raft apply and recovery contract

* Implement `Proposer`: validate-or-refuse, append to the log, return the index; `WaitApplied` blocks
  until the entry is applied on this node. A follower returns `kNotLeader` with the leader's id.
* After an entry is **committed**, call `StateMachine::Apply(index, payload)` once per index, in order,
  from one thread, on every node.
* `Apply` throws only if storage fails. Then do not advance the apply index: retry, or stop the node.
  Never skip the entry.
* Persist the last applied index with the Raft state. On recovery, reconcile it against the durable
  storage sequence and the committed log prefix, then resume from the first unapplied entry. This
  handles a crash between the storage WAL sync and the Raft applied-index update.

`RaftNode` persists term/vote/log/commit state, elects one leader, replicates entries with
`AppendEntries`, requires a majority for commit, and applies committed entries in order. A leader
commits an internal no-op in each term so read barriers can satisfy Raft's current-term condition.
`ReferenceConsensus` remains a deterministic single-node stand-in for tests.

## 5. Behaviours worth knowing

* **Not leader**: `ReplicatedBackend` throws `NotLeaderError(leader_hint)`. `kv_service_impl.cpp` maps
  it to `STATUS_CODE_NOT_LEADER` with the leader hint for PUT, GET, and DELETE. The gRPC boundary test
  checks that follower writes and reads are rejected with that status and hint.
* **Reads (Week 6)**: `ReplicatedBackend::Get` first calls `Proposer::ReadBarrier`, waits for the returned
  committed index to be applied locally, then reads storage. A follower is rejected with `NotLeaderError`.
  `RaftNode` confirms leadership with a quorum heartbeat after a current-term entry is committed;
  `ReferenceConsensus` only provides a local synchronous barrier.
* **Timeout**: if a quorum does not acknowledge before the proposal deadline, the RPC returns
  `STATUS_CODE_UNAVAILABLE`. The entry may still commit later, so retries can add duplicate log entries;
  PUT/DELETE are idempotent at the key-value layer. `request_id` is carried for tracing/future de-duplication.
* **Deletes** always go through the log, even for missing keys (storage needs the tombstone).

## 6. Tests

`tests/consensus/command_test.cpp` (codec, strict decoding, validation), `boundary_test.cpp` (state machine,
proposals without commit, in-order apply, not-leader, apply failure, 8 concurrent proposers, the whole
boundary on top of the Week 4 `LsmStore` with flushes and compactions running), `grpc_boundary_test.cpp`
(client → gRPC → log → storage), and `raft_cluster_test.cpp` (three local gRPC nodes, election, one-follower
quorum, follower catch-up, and leader re-election). The gRPC-dependent suites require Protobuf/gRPC and
GoogleTest and still need to be run in a matching environment.
