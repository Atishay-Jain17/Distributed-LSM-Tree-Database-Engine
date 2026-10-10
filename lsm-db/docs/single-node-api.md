# Single-Node Storage API (Week 2)

Owner: Suhani. Code: `api/`. Tests: `tests/api/`.

## Flow

```
client --gRPC--> KvServiceImpl --StorageBackend--> MemTableBackend --MemTable--> (SkipList later)
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

## What Atishay / Sarthak plug in

1. Make the SkipList implement `MemTable` (or a small adapter) and add it to the factory list at the bottom of
   `tests/api/memtable_test.cpp`: the same contract tests then run against it.
2. In `rpc/node_main.cpp` replace `InMemoryBackend` with
   `std::make_shared<lsmdb::api::MemTableBackend>(std::make_shared<YourSkipListMemTable>())`
   (add `lsmdb_api` to the `lsmdb_node` link libraries).
3. `ReferenceMemTable` (std::map + shared_mutex) is only a stand-in; delete or keep it as a test oracle.

## Prepared for Week 3 (WAL)

`MemTableBackend::Write` is the single place every mutation passes through, in sequence order. WAL integration is:
append (seq, type, key, value) to the WAL **before** `memtable_->Add(...)`, and on startup replay the WAL through
the same `Add`, then set `next_seq_`/`last_visible_` to the highest replayed sequence (a small `Recover(seq)` hook will be needed).

## Running the tests

```bash
cd lsm-db
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```
`api_tests` needs only GoogleTest. `api_grpc_tests` (end-to-end through gRPC) is built when the RPC library is.
