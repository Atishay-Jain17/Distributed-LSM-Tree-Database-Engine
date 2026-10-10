# Startup and Recovery Flow (Week 3)

Owner: Suhani. Code: `recovery/`. Tests: `tests/recovery/`. Depends on `api/` (Week 2).
The WAL itself (format, append, io_uring) is Atishay's / Sarthak's work; recovery only needs the
`WalReader` interface in `recovery/include/lsmdb/recovery/wal_reader.h`.

## Startup order (what `node_main.cpp` should do)

1. Read config (node id, ports, data dir).
2. **Discover SSTables** in the data dir (Vinayak's reader, Week 3/4). Find `flushed_up_to` = the highest
   sequence number stored in any SSTable (0 if there are none).
3. Create an empty `MemTable` and `MemTableBackend`.
4. Open the WAL with the real `WalReader` and call
   `recovery::Recover(reader, backend, flushed_up_to)`.
   * records with sequence <= `flushed_up_to` are skipped (SSTables already hold them)
   * the rest are applied with their ORIGINAL sequence numbers
5. If `stats.dropped_torn_tail`, **truncate the WAL file to `stats.valid_wal_bytes`** before appending anything.
6. Open the WAL for appending. From now on every write goes to the WAL first, then `MemTableBackend`
   (the WAL hook goes in `MemTableBackend::Write`, before `memtable_->Add`).
7. Start the RPC server (`NodeServer::Start`). Nothing is served before recovery finishes.

## Guarantees and policy

| Situation | Behaviour |
|---|---|
| Missing or empty WAL | Start with an empty MemTable |
| Clean log | Every record is applied |
| Last record incomplete or damaged (crash during append) | Dropped; everything before it is applied; `dropped_torn_tail = true` |
| Damaged record with valid records after it | `RecoveryError`: the node refuses to start (silent data loss is worse than not starting) |
| Sequence numbers not strictly increasing | `RecoveryError` |
| `Recover` on a backend that already has data | `RecoveryError` |

After a crash the database contains exactly the writes whose WAL records were completely written.
A write is only acknowledged to a client after its WAL record is durable (Atishay's fsync/io_uring policy decides
what "durable" means; document it next to the WAL). Deletes are tombstones, so recovered deletes still hide older values.
Sequence numbers continue from the highest recovered value + 1, so versions and snapshot reads behave as before the crash.

## What the tests prove (`tests/recovery/recovery_test.cpp`)

* crash at **every byte offset** of a log: recovered state == state after the last complete record
* torn tail dropped, truncated, and appending again works across a second restart
* damaged last record = torn tail; damage before the end = refuse to start
* several versions, deletes of existing and never-seen keys, empty and binary values
* skipped records below `flushed_up_to`
* 3000 random operations + restart + 500 more + restart, compared with an independent `std::map` model

## Integration TODOs

* **Atishay:** implement `WalReader` for the real WAL (return `kTruncatedTail` only for a bad LAST record) and add it to the
  test file next to `ReferenceWalReader`; call `Recover` from `node_main.cpp` (see order above).
* **Sarthak:** none; the I/O path does not change recovery, only what "durable" means.
* **Vinayak (Week 4):** provide `flushed_up_to` from the SSTable manifest.
* `ReferenceWalWriter/Reader` are test stand-ins and can be deleted once the real WAL has the same tests.
