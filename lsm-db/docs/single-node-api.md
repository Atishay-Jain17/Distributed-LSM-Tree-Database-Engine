# Single-Node Storage API (Week 2)

Owner: Suhani. Code: `api/`. Tests: `tests/api/`.

## Flow

```
client --gRPC--> KvServiceImpl --StorageBackend--> MemTableBackend --MemTable--> Atishay SkipList adapter
                  (validation)                      (sequence numbers,            (WAL + SSTables
                                                     tombstones, snapshots)         in Weeks 3-4)
```

`MemTableBackend` implements `rpc::StorageBackend`, so nothing in `rpc/` changes. It depends only on the
`MemTable` interface (`api/include/lsmdb/api/memtable.h`).

## Behaviour

| Operation | Result |
|---|---|
| `Put(k, v)` | Always succeeds. Writes a new version with the next sequence number; older versions stay. `v` may be empty. |
| `Get(k)` | Newest visible value. `nullopt` (-> `NOT_FOUND`) if the key was never written **or** its newest entry is a tombstone. |
| `Delete(k)` | Idempotent (-> `OK` even for a missing key). Always writes a tombstone: it must hide older values that may live in SSTables later. |
| Put after Delete | Key comes back with the new value. |

Key/value size and empty-key validation stay in the RPC layer (`validation.h`); the API layer assumes valid input.

## Versions and snapshots

* Sequence numbers start at 1 and are consecutive (`LastSequence()` = last fully visible write; 0 = empty).
* A write is assigned its number and inserted under one lock, then published; readers use `LastSequence()`
  as their read timestamp, so they never see a half-applied write.
* `GetAt(key, seq)` returns the newest entry with sequence <= `seq` and says `kFound`, `kDeleted` or `kNotFound`.
  `kDeleted` vs `kNotFound` matters from Week 3: `kNotFound` means "look in SSTables", `kDeleted` means "stop, it is deleted".

## Combined root build integration

The root build enables `AtishaySkipListMemTable`, a locked adapter that stores each version under a
binary-safe composite key (user key, then inverted sequence), and encodes tombstones separately from
values. It implements snapshots and ordered `ForEach`, so Suhani's `LsmStore` uses the existing
Atishay SkipList when the combined root project is built. The standalone `lsm-db` subproject keeps
`ReferenceMemTable` as its default because the root SkipList header is outside that subproject.

The adapter contract is exercised by the root `NodeStorageSmoke` test for binary keys/values,
multiple versions, tombstones, traversal order, out-of-order sequence rejection, and concurrent
API access.

## WAL integration

`MemTableBackend` remains the in-memory API and test oracle. The runtime node uses `LsmStore`;
`PersistentBackend` installs its `before_write` hook to append `(seq, type, key, value)` to the WAL
before changing the active MemTable. On startup it discovers the persisted-table high-water mark,
replays the WAL through `Restore`, truncates an incomplete tail, and only then starts the RPC server.

## Running the tests

```bash
cd lsm-db
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```
`api_tests` needs only GoogleTest. `api_grpc_tests` (end-to-end through gRPC) is built when the RPC library is.
