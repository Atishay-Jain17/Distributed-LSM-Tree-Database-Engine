# Single-node flow: WAL → MemTable → SSTables → compaction (Week 4)

Owner: Suhani. Code: `lifecycle/` (`LsmStore`, `TableStore`, `ReferenceTableStore`),
tests: `tests/lifecycle/`. Builds on Week 2 (`api/`) and Week 3 (`recovery/`).

## 1. The whole path

```
            PUT / DELETE                              GET
                 │                                     │
   (WAL append — Atishay/Vinayak's WAL, Week 2-3)      │  one snapshot sequence for the whole read
                 ▼                                     ▼
        ┌─────────────────┐   rotate at size    ┌──────────────┐
        │ ACTIVE MemTable │ ──────────────────► │ IMMUTABLE    │  (at most one)
        └─────────────────┘                     │ MemTable     │
                 ▲                              └──────┬───────┘
     reads: 1st  │                         reads: 2nd  │ flush worker: TableStore::AddTable
                                                       ▼
                                          ┌──────────────────────┐
                              reads: 3rd  │ TABLES (SSTables)    │  newest → oldest
                                          └──────────┬───────────┘
                                                     │ compaction worker: TableStore::Compact
                                                     ▼
                                          merged tables, old files deleted
```

A read checks active, then immutable, then the tables, and the first layer that has the key
(value **or tombstone**) decides the answer. A tombstone in a newer layer hides a value in an
older one.

## 2. Components and who plugs in

| Piece | Where | Real implementation by |
|---|---|---|
| Active / immutable MemTable | `api::MemTable` (`ForEach` added for flushing) | Atishay (SkipList) |
| Rotation, flush worker, compaction scheduling, snapshots | `lifecycle::LsmStore` | Suhani |
| SSTable writer, sparse index, Bloom filter, merge, file lifecycle | `lifecycle::TableStore` | Atishay / Sarthak / Vinayak |
| Stand-in so everything is testable now | `lifecycle::ReferenceTableStore` | Suhani |
| WAL replay at start-up | `recovery::Recover` (Week 3) | Suhani |

The real storage code only has to implement the four methods of `TableStore`
(`AddTable`, `Get`, `NeedsCompaction`, `Compact`). Nothing else changes.

## 3. Rotation (Fri)

* Threshold: `LsmOptions::memtable_bytes` against `MemTable::ApproximateMemoryUsage()`.
* Checked on every write. When reached, the active MemTable becomes the immutable one and a new
  empty active MemTable is created (instant: no disk I/O under the write lock).
* Only **one** immutable MemTable exists. If the active one fills up again before the flush
  finished, the writer **stalls** until the flush completes (backpressure; memory stays bounded).
  `LsmStats::write_stalls` counts how often this happens.
* Rotation boundary is recorded as a sequence number (`imm_last_seq_`).

## 4. Flush (Sat)

1. Flush worker takes the immutable MemTable and calls `TableStore::AddTable`.
2. The table store writes a new table (temp name, then rename) and publishes it.
3. Only **after** that does `LsmStore` drop the immutable MemTable. A read can therefore never
   find the data in neither place.
4. `on_flushed(seq)` fires, then `FlushedUpTo()` becomes `seq`. The WAL owner may delete log
   records up to `seq`. This is the same number `recovery::Recover(..., flushed_up_to)` expects.
5. If `AddTable` throws: the immutable MemTable is kept (data stays readable and in the WAL),
   `background_error()` is set, `FlushedUpTo()` does not move, and writers that need a rotation
   get `std::runtime_error`.

## 5. Compaction scheduling (Sun)

* After each successful flush `NeedsCompaction()` is asked; if true the compaction worker runs
  `Compact(oldest_snapshot)`, and repeats while the store still asks for it.
* One flush and one compaction can run at the same time as reads and writes.
* A failing compaction is recorded and not retried automatically; writes keep working.

### Snapshots and compaction (important contract)

A read at sequence `s` must see "the newest version with sequence ≤ s". Compaction may only throw
away versions no read can still ask for. So `LsmStore` passes `oldest_snapshot` (the smallest
sequence of any running read or any `Snapshot` object, else the latest sequence) and the table
store must:

* keep **all** versions with sequence > `oldest_snapshot`;
* keep the **newest** version with sequence ≤ `oldest_snapshot`;
* drop older ones; drop a tombstone only if it is at or below `oldest_snapshot` and nothing older
  than the output remains.

Use `auto snap = db.AcquireSnapshot(); db.GetAt(key, snap.sequence());` for a repeatable read.

## 6. File lifecycle rules (Wed)

1. Write to `NNNNNN.tmp`, flush, rename to `NNNNNN.sst`. A `.sst` is always complete; leftover
   `.tmp` files are deleted at start-up.
2. Publish a new table list with one pointer swap (copy-on-write). Readers keep the list they
   started with.
3. A replaced file is deleted by the **last reader** that stops using it (reference counting), not
   by the compaction. `ObsoleteFileSurvivesUntilLastReaderIsDone` tests this.
4. Tables flushed while a compaction is running stay **newer** than the compaction output.

## 7. Tests (Mon–Wed) — `tests/lifecycle/lifecycle_test.cpp`

19 tests: rotation threshold; flush → table; `FlushedUpTo`/`on_flushed`; reads see the immutable
MemTable while a flush is blocked; delete across layers; write stall and release; flush failure;
concurrent writers + readers across many flushes; compaction scheduling; readers during
compaction (no lost write, no key disappearing, no going backwards); snapshot kept alive across
compaction; tombstone dropping; compaction failure; file creation/replacement/cleanup; obsolete
file lives until its last reader; flush landing during a compaction; Restore/start sequence;
shutdown with work pending. All pass under ThreadSanitizer (15 repeated runs) and
AddressSanitizer + UBSan.

## 8. Known limits and hand-off notes

* `ReferenceTableStore` scans files and does not reload tables at start-up. The real manifest must
  tell `LsmStore` the highest persisted sequence (`LsmOptions::start_sequence`) so recovery and new
  writes continue correctly.
* The WAL is not appended inside `LsmStore` yet: the node wiring should append to the WAL first,
  then call `Put`/`Delete`, exactly as in `docs/recovery-flow.md`. `on_flushed` is the hook for
  WAL truncation.
* `~LsmStore` does not flush the active/immutable MemTables; their data is recovered from the WAL.
  Call `Flush()` first for a clean shutdown.
* The SkipList MemTable must implement `MemTable::ForEach` (key ascending, sequence descending).
* `recovery::Recover` now takes an `api::RestoreTarget` (implemented by both `MemTableBackend` and `LsmStore`). It accepts a store whose `LastSequence()` is at most `flushed_up_to`, so a store opened with `start_sequence` can be recovered. `tests/lifecycle/single_node_flow_test.cpp` runs the whole path: write, flush, compact, crash, recover.
* Only one compaction runs at a time (single worker). Multi-threaded leveled compaction is
  Vinayak's `TableStore::Compact`.
