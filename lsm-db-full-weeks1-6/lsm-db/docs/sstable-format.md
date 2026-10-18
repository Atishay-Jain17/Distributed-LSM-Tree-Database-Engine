# SSTable format v1 (Week 3)

The authoritative description is the comment block at the top of
`storage/include/lsmdb/storage/sstable_format.h`. Summary:

```
[ data section: records sorted by (key asc, seq desc) ][ pad to 4096 ]
[ sparse index section ][ Bloom filter section ]
[ footer: exactly 4096 bytes, always the last page ]
```

* **Record:** `u32 key_len | u32 value_len | u64 seq | u8 type | key | value` (little-endian).
* **Footer:** magic `LSMSST01`, version, entry count, min/max key, min/max seq, data size,
  data CRC32, sparse-index offset/size, Bloom-filter offset/size, footer CRC32.
* **Sparse index:** the writer samples every 64th record at record boundaries, stores each
  sample's key and data offset, and assigns the byte range through the next sample. The
  index is serialized in little-endian form in its own aligned section. `SSTableReader::Get` binary-searches the
  index and reads only the candidate block; legacy files with no index use a sequential scan.
* **Direct I/O:** every section starts on a 4096-byte boundary and the file size is a multiple
  of 4096, so Sarthak's O_DIRECT layer can read any section with aligned offsets and lengths.
* **Crash safety:** the writer writes `<name>.tmp`, fsyncs, renames to `<name>`, then fsyncs the
  directory. A reader sees no file or a complete file.
* **Integrity:** the footer is checked at `Open()`; `VerifyChecksum()` checks the data CRC. Compaction
  verifies every input before merging.

**Bloom filter:** new tables serialize a versioned Bloom filter after the sparse index. It uses
seven double-hashed probes and ten bits per input record, with an internal CRC and zero padding
to the 4096-byte boundary. `SSTableReader::Get` checks it before reading the indexed data block.
False positives only cause an unnecessary lookup; false negatives are forbidden by the tests.
Legacy v1 tables with no filter remain readable because both section offsets are optional footer
fields and are validated independently.

**Known limitation:** the index is a sparse key-to-byte-range map; the reader still scans
records inside the selected range. `VerifyChecksum()` scans the complete data section.
