# Leveled compaction (Week 4)

**Levels.** L0 holds files flushed from MemTables (ranges may overlap). L1..L6 hold sorted,
non-overlapping files. Budgets: L0 triggers at 4 files; L1 = 10 MiB; each deeper level 10x.
All numbers are in `CompactionOptions`.

**Picking.** Score = files/trigger (L0) or bytes/budget (L1+); the highest score >= 1 wins. L0
compacts all its files together with the overlapping L1 files. A deeper level compacts one file
(round-robin by key) with the overlapping files one level down.

**Merging.** A k-way merge (min-heap on key asc, seq desc). Per user key, newest first:

1. keep every version with `seq > oldest_snapshot` (a live reader may need it);
2. keep the newest version with `seq <= oldest_snapshot`, **unless** it is a tombstone and no deeper
   level can hold the key, in which case the tombstone is dropped;
3. drop every older version.

Output files roll over near `target_file_bytes`, only on key boundaries, so a key never straddles
two files and L1+ ranges stay disjoint.

**Reads stay correct.** `VersionGet` checks every candidate file and picks the highest `seq <= snapshot`,
so it does not rely on "which level is newer".

**File lifetime.** Compaction publishes a new `Version` atomically, then marks the replaced files
obsolete. A file is deleted in `FileMeta`'s destructor, i.e. only after the last `Version` or reader
referencing it is gone. A read that began before the compaction can always finish.

**Failure handling.** Inputs are checksum-verified first. On any error, partial outputs are deleted
and the `Version` is untouched. The background worker records `last_error()` and stays alive.

**Recovery.** File names encode the level (`L1-000012.sst`). `VersionSet::Recover()` rebuilds levels,
removes `.tmp` files and resumes file numbering.

**Known limitations (state these in the viva):** no manifest, so a crash between publishing outputs and
deleting inputs can leave both on disk (data is still correct; the no-overlap rule may be violated until
the next compaction); no trivial-move optimization; one compaction at a time.
