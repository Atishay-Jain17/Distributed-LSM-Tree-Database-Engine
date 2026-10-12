# SSTable format v1 (Week 3)

The authoritative description is the comment block at the top of
`storage/include/lsmdb/storage/sstable_format.h`. Summary:

```
[ data section: records sorted by (key asc, seq desc) ][ pad to 4096 ]
[ (reserved) index section ][ (reserved) filter section ]    <- Atishay / Sarthak, Week 4
[ footer: exactly 4096 bytes, always the last page ]
```

* **Record:** `u32 key_len | u32 value_len | u64 seq | u8 type | key | value` (little-endian).
* **Footer:** magic `LSMSST01`, version, entry count, min/max key, min/max seq, data size,
  data CRC32, offsets/sizes of the reserved index and filter sections (0 today), footer CRC32.
* **Direct I/O:** every section starts on a 4096-byte boundary and the file size is a multiple
  of 4096, so Sarthak's O_DIRECT layer can read any section with aligned offsets and lengths.
* **Crash safety:** the writer writes `<name>.tmp`, fsyncs, renames to `<name>`, then fsyncs the
  directory. A reader sees no file or a complete file.
* **Integrity:** the footer is checked at `Open()`; `VerifyChecksum()` checks the data CRC. Compaction
  verifies every input before merging.

**Adding the sparse index / Bloom filter (Week 4 owners):** append the new section(s) between the
data section and the footer, each padded to 4096, and fill `index_offset/size` and
`filter_offset/size` in the footer. `SSTableReader::Get` is the single place that would use them.
Bump `kFormatVersion` only if the layout of existing fields changes.

**Known limitation:** `Get` is a sequential scan until the sparse index exists.
