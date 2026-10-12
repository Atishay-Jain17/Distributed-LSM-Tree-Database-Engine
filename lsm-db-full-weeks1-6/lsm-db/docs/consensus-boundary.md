# Consensus ↔ RPC ↔ storage boundary (Week 5)

Owner: Suhani. Code: `consensus/` (`Command`, `StateMachine`, `Proposer`, `StorageStateMachine`,
`ReferenceConsensus`, `ReplicatedBackend`), tests: `tests/consensus/`. Raft itself (roles,
elections, log replication) is Atishay's; this layer is what Raft replicates and where the
replicated entries land.

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

* **RPC** knows only `StorageBackend`. Swapping the plain backend for `ReplicatedBackend` in
  `node_main.cpp` turns a single-node database into a replicated one; the gRPC services do not change.
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

## 4. What Raft must do (contract for the real implementation)

* Implement `Proposer`: validate-or-refuse, append to the log, return the index; `WaitApplied` blocks
  until the entry is applied on this node. A follower returns `kNotLeader` with the leader's id.
* After an entry is **committed**, call `StateMachine::Apply(index, payload)` once per index, in order,
  from one thread, on every node.
* `Apply` throws only if storage fails. Then do not advance the apply index: retry, or stop the node.
  Never skip the entry.
* Persist the last applied index together with storage progress and pass it to
  `StorageStateMachine(storage, last_applied)` after a restart. Indexes at or below it are skipped,
  so replaying the log after a crash is safe. (`FlushedUpTo()` of `LsmStore` and `Recover` from Week 3
  cover the storage side of this.)

`ReferenceConsensus` is a single-node stand-in with the same behaviour (log, in-order apply, stop on
apply failure, optional manual commit) used to build and test everything above.

## 5. Behaviours worth knowing

* **Not leader**: `ReplicatedBackend` throws `NotLeaderError(leader_hint)`. `kv_service_impl.cpp` currently
  reports unknown exceptions as `STATUS_CODE_INTERNAL`; mapping this exception to `STATUS_CODE_NOT_LEADER`
  with `leader_hint` (already in the proto) is a small follow-up for the RPC layer.
* **Reads**: `Get` reads the local storage. On the leader this is current up to the last applied entry; on a
  follower it can be behind. Linearizable reads (read-index or leader lease) are not part of Week 5.
* **Timeout**: if a proposed write is not applied within `apply_timeout` (default 5 s), `Put/Delete` throws.
  The write may still commit later, so a client that retries should be prepared for a duplicate; `request_id`
  is carried in the command for future de-duplication but nothing uses it yet.
* **Deletes** always go through the log, even for missing keys (storage needs the tombstone).

## 6. Tests

`tests/consensus/command_test.cpp` (codec, strict decoding, validation), `boundary_test.cpp` (state machine,
proposals without commit, in-order apply, not-leader, apply failure, 8 concurrent proposers, the whole
boundary on top of the Week 4 `LsmStore` with flushes and compactions running) and, when the RPC library is
built, `grpc_boundary_test.cpp` (client → gRPC → log → storage, binary keys, 1 MiB value, protobuf round trip
of the encoded bytes).
