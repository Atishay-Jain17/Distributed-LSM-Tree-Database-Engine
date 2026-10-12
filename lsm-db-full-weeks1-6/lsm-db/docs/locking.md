# MemTable locking design (Week 2)

**One `std::shared_mutex` per MemTable.**

| Operation | Lock | Why |
|---|---|---|
| `Get`, `Entries`, `EntryCount`, `ApproximateBytes`, `frozen` | shared | many lookups run in parallel |
| `Put`, `Delete`, `Freeze` | exclusive | one writer, nobody reads half-inserted state |

**Why one lock?** Lookups are very short and writes are already serialized by the WAL, so a
single reader/writer lock is simple, provably correct and easy to explain. A finer-grained or
lock-free SkipList is possible later, but it is an optimization, not a requirement.

**Versions (MVCC hook).** Each write carries a sequence number (the Raft log index).
Ordering is `(key ascending, seq descending)`. `Get(key, snapshot)` returns the newest version
with `seq <= snapshot`; a tombstone is reported as `kDeleted`, not as missing. Writing the same
`(key, seq)` twice overwrites, so replaying a log entry is harmless.

**Freezing.** `Freeze()` takes the exclusive lock and sets a flag; afterwards writes return
`false`. Because the flag is checked under the same lock as the insert, no write can slip in
after `Freeze()` returns. The flush path then reads `Entries()` while readers continue.

**Swapping in the SkipList.** Replace the `std::map table_` member and the three places that touch
it (`Insert`, `Get`, `Entries`). Locking, freezing and versioning rules stay unchanged.

**Measuring contention.** `build/benchmarks/memtable_contention` compares the shared lock with an
exclusive-only lock (read-heavy 95% and mixed 50%). Run it on a multi-core machine; on a
single-core machine every configuration looks the same.

**Verified by** `tests/storage/memtable_test.cpp`: no lost writes with 8 writers, monotonic
consistent reads while a writer runs, a clean Freeze cut-off under load. The suite also ran
clean under ThreadSanitizer.
