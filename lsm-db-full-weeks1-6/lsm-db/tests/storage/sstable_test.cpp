#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lsmdb/storage/crc32.h"
#include "lsmdb/storage/flush.h"
#include "lsmdb/storage/sstable_reader.h"
#include "lsmdb/storage/sstable_writer.h"
#include "temp_dir.h"

using namespace lsmdb::storage;
using lsmdb::testing_support::TempDir;

namespace {
Entry Put(std::string k, std::uint64_t seq, std::string v) { return Entry{std::move(k), seq, EntryType::kPut, std::move(v)}; }
Entry Del(std::string k, std::uint64_t seq) { return Entry{std::move(k), seq, EntryType::kDelete, ""}; }

SSTableMetadata WriteFile(const std::string& path, const std::vector<Entry>& entries) {
  SSTableWriter w(path);
  for (const auto& e : entries) w.Add(e);
  return w.Finish();
}

void FlipByte(const std::string& path, std::uint64_t offset) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(offset));
  char c;
  f.read(&c, 1);
  c = static_cast<char>(c ^ 0x5a);
  f.seekp(static_cast<std::streamoff>(offset));
  f.write(&c, 1);
}
}  // namespace

TEST(Crc32, MatchesKnownVectorAndChains) {
  EXPECT_EQ(Crc32Update(0, "123456789", 9), 0xCBF43926u);
  auto whole = Crc32Update(0, "hello world", 11);
  auto chained = Crc32Update(Crc32Update(0, "hello ", 6), "world", 5);
  EXPECT_EQ(whole, chained);
}

TEST(SSTable, RoundTripWithVersionsAndTombstones) {
  TempDir dir;
  std::vector<Entry> in = {Put("apple", 9, "red"), Put("apple", 3, "green"), Del("banana", 7),
                           Put("banana", 2, "yellow"), Put("cherry", 4, "dark")};
  auto meta = WriteFile(dir.File("a.sst"), in);
  EXPECT_EQ(meta.entry_count, 5u);
  EXPECT_EQ(meta.min_key, "apple");
  EXPECT_EQ(meta.max_key, "cherry");
  EXPECT_EQ(meta.min_seq, 2u);
  EXPECT_EQ(meta.max_seq, 9u);

  auto r = SSTableReader::Open(dir.File("a.sst"));
  EXPECT_EQ(r->metadata().entry_count, 5u);
  ASSERT_NO_THROW(r->VerifyChecksum());

  std::vector<Entry> out;
  for (auto it = r->NewIterator(); it->Valid(); it->Next()) out.push_back(it->entry());
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(out[i].key, in[i].key);
    EXPECT_EQ(out[i].seq, in[i].seq);
    EXPECT_EQ(out[i].type, in[i].type);
    EXPECT_EQ(out[i].value, in[i].value);
  }
}

TEST(SSTable, GetHonoursSnapshotsAndReportsTombstones) {
  TempDir dir;
  WriteFile(dir.File("a.sst"), {Put("apple", 9, "red"), Put("apple", 3, "green"), Del("banana", 7), Put("banana", 2, "yellow")});
  auto r = SSTableReader::Open(dir.File("a.sst"));
  EXPECT_EQ(r->Get("apple")->value, "red");
  EXPECT_EQ(r->Get("apple", 8)->value, "green");
  EXPECT_FALSE(r->Get("apple", 2).has_value());
  auto b = r->Get("banana");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->type, EntryType::kDelete);
  EXPECT_EQ(r->Get("banana", 6)->value, "yellow");
  EXPECT_FALSE(r->Get("aaa").has_value());       // before min_key
  EXPECT_FALSE(r->Get("zzz").has_value());       // after max_key
  EXPECT_FALSE(r->Get("avocado").has_value());   // inside range, absent
}

TEST(SSTable, BinaryKeysAndValuesSurvive) {
  TempDir dir;
  std::string key("a\0b\xff", 4), value("\0\0\x01\0", 4);
  WriteFile(dir.File("a.sst"), {Put(key, 1, value)});
  auto r = SSTableReader::Open(dir.File("a.sst"));
  auto e = r->Get(key);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value, value);
}

TEST(SSTable, LayoutIsAlignedForDirectIo) {
  TempDir dir;
  auto meta = WriteFile(dir.File("a.sst"), {Put("k", 1, std::string(5000, 'x'))});
  EXPECT_EQ(std::filesystem::file_size(dir.File("a.sst")) % kBlockAlignment, 0u);
  EXPECT_EQ(std::filesystem::file_size(dir.File("a.sst")), meta.file_size);
  auto r = SSTableReader::Open(dir.File("a.sst"));
  // reserved sections are empty today; the data section is padded to a full block
  EXPECT_EQ(r->footer().index_size, 0u);
  EXPECT_EQ(r->footer().filter_size, 0u);
  EXPECT_EQ(meta.file_size, AlignUp(r->footer().data_size) + kFooterSize);
}

TEST(SSTable, WriterEnforcesOrderAndLimits) {
  TempDir dir;
  SSTableWriter w(dir.File("a.sst"));
  w.Add(Put("b", 5, "v"));
  EXPECT_THROW(w.Add(Put("a", 9, "v")), std::invalid_argument);   // key goes backwards
  EXPECT_THROW(w.Add(Put("b", 5, "v")), std::invalid_argument);   // duplicate (key, seq)
  EXPECT_THROW(w.Add(Put("b", 6, "v")), std::invalid_argument);   // seq must DEscend within a key
  EXPECT_THROW(w.Add(Put("", 1, "v")), std::invalid_argument);    // empty key
  EXPECT_THROW(w.Add(Put(std::string(kMaxSSTableKeyBytes + 1, 'k'), 1, "v")), std::invalid_argument);
  EXPECT_THROW(w.Add(Entry{"c", 1, EntryType::kDelete, "not-empty"}), std::invalid_argument);
  EXPECT_NO_THROW(w.Add(Put("b", 4, "v")));
}

TEST(SSTable, EmptyFinishThrowsAndAbandonedWriterLeavesNothing) {
  TempDir dir;
  {
    SSTableWriter w(dir.File("a.sst"));
    EXPECT_THROW(w.Finish(), std::logic_error);
  }
  {
    SSTableWriter w(dir.File("b.sst"));
    w.Add(Put("k", 1, "v"));
  }  // destroyed without Finish
  EXPECT_FALSE(std::filesystem::exists(dir.File("a.sst")));
  EXPECT_FALSE(std::filesystem::exists(dir.File("b.sst")));
  EXPECT_FALSE(std::filesystem::exists(dir.File("a.sst.tmp")));
  EXPECT_FALSE(std::filesystem::exists(dir.File("b.sst.tmp")));
}

TEST(SSTable, FinishPublishesAtomically) {
  TempDir dir;
  SSTableWriter w(dir.File("a.sst"));
  w.Add(Put("k", 1, "v"));
  EXPECT_FALSE(std::filesystem::exists(dir.File("a.sst")));   // not visible yet
  EXPECT_TRUE(std::filesystem::exists(dir.File("a.sst.tmp")));
  w.Finish();
  EXPECT_TRUE(std::filesystem::exists(dir.File("a.sst")));
  EXPECT_FALSE(std::filesystem::exists(dir.File("a.sst.tmp")));
}

TEST(SSTable, DetectsCorruption) {
  TempDir dir;
  WriteFile(dir.File("a.sst"), {Put("apple", 1, "red"), Put("banana", 1, "yellow")});

  // 1. flipped byte in the data section: Open succeeds, checksum verification fails
  std::filesystem::copy_file(dir.File("a.sst"), dir.File("data.sst"));
  FlipByte(dir.File("data.sst"), 20);
  auto r = SSTableReader::Open(dir.File("data.sst"));
  EXPECT_THROW(r->VerifyChecksum(), CorruptionError);

  // 2. flipped byte in the footer: Open fails
  std::filesystem::copy_file(dir.File("a.sst"), dir.File("footer.sst"));
  FlipByte(dir.File("footer.sst"), std::filesystem::file_size(dir.File("a.sst")) - kFooterSize + 17);
  EXPECT_THROW(SSTableReader::Open(dir.File("footer.sst")), CorruptionError);

  // 3. truncated file
  std::filesystem::copy_file(dir.File("a.sst"), dir.File("trunc.sst"));
  std::filesystem::resize_file(dir.File("trunc.sst"), 100);
  EXPECT_THROW(SSTableReader::Open(dir.File("trunc.sst")), CorruptionError);

  // 4. not an SSTable at all
  {
    std::ofstream junk(dir.File("junk.sst"), std::ios::binary);
    junk << std::string(kBlockAlignment * 2, 'x');
  }
  EXPECT_THROW(SSTableReader::Open(dir.File("junk.sst")), CorruptionError);

  // 5. missing file is an I/O error, not corruption
  EXPECT_THROW(SSTableReader::Open(dir.File("nope.sst")), std::runtime_error);
}

TEST(SSTable, LargeFileSpanningManyReadChunks) {
  TempDir dir;
  constexpr int kN = 20000;
  SSTableWriter w(dir.File("big.sst"));
  for (int i = 0; i < kN; ++i) {
    char key[16];
    std::snprintf(key, sizeof key, "key%08d", i);
    w.Add(Put(key, 1, std::string(100, static_cast<char>('a' + i % 26))));
  }
  w.Finish();
  auto r = SSTableReader::Open(dir.File("big.sst"));
  ASSERT_NO_THROW(r->VerifyChecksum());
  int count = 0;
  for (auto it = r->NewIterator(); it->Valid(); it->Next()) ++count;
  EXPECT_EQ(count, kN);
  for (int i : {0, 1, 777, 12345, kN - 1}) {
    char key[16];
    std::snprintf(key, sizeof key, "key%08d", i);
    auto e = r->Get(key);
    ASSERT_TRUE(e.has_value()) << key;
    EXPECT_EQ(e->value.size(), 100u);
  }
}

TEST(Flush, FrozenMemTableBecomesSSTable) {
  TempDir dir;
  MemTable t;
  ASSERT_TRUE(t.Put("b", "2", 2));
  ASSERT_TRUE(t.Put("a", "1", 1));
  ASSERT_TRUE(t.Delete("c", 3));
  EXPECT_THROW(FlushMemTableToSSTable(t, dir.File("f.sst")), std::logic_error);  // not frozen
  t.Freeze();
  auto meta = FlushMemTableToSSTable(t, dir.File("f.sst"));
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->entry_count, 3u);
  auto r = SSTableReader::Open(dir.File("f.sst"));
  EXPECT_EQ(r->Get("a")->value, "1");
  EXPECT_EQ(r->Get("b")->value, "2");
  EXPECT_EQ(r->Get("c")->type, EntryType::kDelete);
}

TEST(Flush, EmptyMemTableCreatesNoFile) {
  TempDir dir;
  MemTable t;
  t.Freeze();
  EXPECT_FALSE(FlushMemTableToSSTable(t, dir.File("f.sst")).has_value());
  EXPECT_FALSE(std::filesystem::exists(dir.File("f.sst")));
}
