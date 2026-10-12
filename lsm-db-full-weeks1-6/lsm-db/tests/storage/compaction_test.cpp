#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "lsmdb/storage/compaction_manager.h"
#include "lsmdb/storage/sstable_writer.h"
#include "lsmdb/storage/version_set.h"
#include "temp_dir.h"

using namespace lsmdb::storage;
using lsmdb::testing_support::TempDir;
namespace fs = std::filesystem;

namespace {
Entry Put(std::string k, std::uint64_t seq, std::string v) { return Entry{std::move(k), seq, EntryType::kPut, std::move(v)}; }
Entry Del(std::string k, std::uint64_t seq) { return Entry{std::move(k), seq, EntryType::kDelete, ""}; }

// entries must already be in EntryLess order
std::shared_ptr<FileMeta> AddTable(VersionSet& vs, int level, const std::vector<Entry>& entries) {
  std::uint64_t number = vs.NewFileNumber();
  SSTableWriter w(vs.FilePath(level, number));
  for (const auto& e : entries) w.Add(e);
  return vs.AddFile(level, number, w.Finish());
}

std::size_t CountSst(const std::string& dir) {
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) n += e.path().extension() == ".sst";
  return n;
}
std::size_t CountTmp(const std::string& dir) {
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) n += e.path().extension() == ".tmp";
  return n;
}

std::vector<Entry> AllEntries(const Version& v, int level) {
  std::vector<Entry> out;
  for (const auto& f : v.levels[level]) {
    for (auto it = f->Reader()->NewIterator(); it->Valid(); it->Next()) out.push_back(it->entry());
  }
  return out;
}

CompactionOptions SmallOptions() {
  CompactionOptions o;
  o.l0_file_trigger = 4;
  o.level1_max_bytes = 1 << 20;
  o.target_file_bytes = 1 << 20;
  return o;
}
}  // namespace

// ------------------------------------------------------------------ picker

TEST(Picker, NothingToDoBelowThresholds) {
  TempDir dir;
  VersionSet vs(dir.path());
  for (int i = 0; i < 3; ++i) AddTable(vs, 0, {Put("k" + std::to_string(i), 1 + i, "v")});
  CompactionPicker picker(SmallOptions());
  EXPECT_FALSE(picker.Pick(vs.Current()).has_value());
}

TEST(Picker, L0AtTriggerTakesAllL0FilesAndOverlappingL1) {
  TempDir dir;
  VersionSet vs(dir.path());
  AddTable(vs, 1, {Put("a", 1, "v"), Put("b", 1, "v")});   // overlaps
  AddTable(vs, 1, {Put("x", 1, "v"), Put("y", 1, "v")});   // does not overlap
  for (int i = 0; i < 4; ++i) AddTable(vs, 0, {Put("a", 10 + i, "v"), Put("c", 10 + i, "v")});
  CompactionPicker picker(SmallOptions());
  auto task = picker.Pick(vs.Current());
  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task->level, 0);
  EXPECT_EQ(task->output_level, 1);
  EXPECT_EQ(task->inputs.size(), 4u);
  ASSERT_EQ(task->next_inputs.size(), 1u);
  EXPECT_EQ(task->next_inputs[0]->meta.min_key, "a");
}

TEST(Picker, OversizedLevelPicksOneFileRoundRobin) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionOptions o = SmallOptions();
  o.level1_max_bytes = 1;  // any L1 content is over budget
  AddTable(vs, 1, {Put("a", 1, "v")});
  AddTable(vs, 1, {Put("m", 1, "v")});
  CompactionPicker picker(o);
  auto t1 = picker.Pick(vs.Current());
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(t1->level, 1);
  ASSERT_EQ(t1->inputs.size(), 1u);
  EXPECT_EQ(t1->inputs[0]->meta.min_key, "a");
  auto t2 = picker.Pick(vs.Current());
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->inputs[0]->meta.min_key, "m");  // moved on, not starved
}

// ------------------------------------------------------------------ merging

TEST(Compaction, MergesOverlappingL0IntoSortedNonOverlappingL1) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  std::map<std::string, std::string> truth;
  std::uint64_t seq = 1;
  for (int f = 0; f < 4; ++f) {  // 4 flushes, each rewriting overlapping keys
    std::vector<Entry> entries;
    for (int k = 0; k < 50; ++k) {
      char key[16];
      std::snprintf(key, sizeof key, "key%03d", k);
      std::string val = "f" + std::to_string(f) + "-" + key;
      entries.push_back(Put(key, seq++, val));
      truth[key] = val;
    }
    AddTable(vs, 0, entries);
  }
  auto stats = mgr.RunOnce();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->input_files, 4u);
  EXPECT_EQ(stats->entries_in, 200u);
  EXPECT_EQ(stats->entries_out, 50u);      // only the newest version of each key survives
  EXPECT_EQ(stats->entries_dropped, 150u);

  auto v = vs.Current();
  EXPECT_EQ(v->NumFiles(0), 0u);
  EXPECT_GE(v->NumFiles(1), 1u);
  auto l1 = AllEntries(*v, 1);
  for (std::size_t i = 1; i < l1.size(); ++i) EXPECT_TRUE(EntryLess(l1[i - 1], l1[i]));  // sorted
  for (const auto& [k, val] : truth) {
    auto e = VersionGet(*v, k);
    ASSERT_TRUE(e.has_value()) << k;
    EXPECT_EQ(e->value, val) << k;
  }
}

TEST(Compaction, ReplacedFilesAreDeletedOnlyWhenNoVersionUsesThem) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  for (int f = 0; f < 4; ++f) AddTable(vs, 0, {Put("k", 10 + f, "v" + std::to_string(f))});
  EXPECT_EQ(CountSst(dir.path()), 4u);

  auto held = vs.Current();   // a reader that started before the compaction
  ASSERT_TRUE(mgr.RunOnce().has_value());
  EXPECT_EQ(CountSst(dir.path()), 5u);        // 4 old (still referenced) + 1 new
  EXPECT_EQ(VersionGet(*held, "k")->value, "v3");  // old Version still fully readable

  held.reset();
  EXPECT_EQ(CountSst(dir.path()), 1u);        // now the old files are gone
  EXPECT_EQ(VersionGet(*vs.Current(), "k")->value, "v3");
}

TEST(Compaction, TombstoneDroppedWhenNothingDeeperButKeptWhenDeeperDataExists) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  AddTable(vs, 3, {Put("shadowed", 1, "old")});            // deeper data for "shadowed"
  AddTable(vs, 0, {Put("gone", 2, "v"), Put("shadowed", 3, "v")});
  AddTable(vs, 0, {Del("gone", 5), Del("shadowed", 6)});
  AddTable(vs, 0, {Put("other", 7, "x")});
  AddTable(vs, 0, {Put("other2", 8, "y")});
  ASSERT_TRUE(mgr.RunOnce().has_value());
  auto v = vs.Current();
  EXPECT_FALSE(VersionGet(*v, "gone").has_value());        // tombstone + value both erased
  auto s = VersionGet(*v, "shadowed");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->type, EntryType::kDelete);                  // must stay: hides the L3 value
  EXPECT_EQ(s->seq, 6u);
}

TEST(Compaction, KeepsVersionsNeededByOldestSnapshot) {
  auto run = [](std::uint64_t oldest_snapshot) {
    TempDir dir;
    VersionSet vs(dir.path());
    CompactionManager mgr(vs, SmallOptions(), [=] { return oldest_snapshot; });
    AddTable(vs, 0, {Put("k", 3, "v3")});
    AddTable(vs, 0, {Put("k", 7, "v7")});
    AddTable(vs, 0, {Put("k", 9, "v9")});
    AddTable(vs, 0, {Put("z", 10, "z")});
    EXPECT_TRUE(mgr.RunOnce().has_value());
    std::vector<std::uint64_t> seqs;
    for (const auto& e : AllEntries(*vs.Current(), 1)) if (e.key == "k") seqs.push_back(e.seq);
    return seqs;
  };
  EXPECT_EQ(run(kMaxSequence), (std::vector<std::uint64_t>{9}));        // no snapshots: newest only
  EXPECT_EQ(run(5), (std::vector<std::uint64_t>{9, 7, 3}));             // 9,7 > 5 kept; 3 is what snapshot 5 sees
  EXPECT_EQ(run(8), (std::vector<std::uint64_t>{9, 7}));                // 7 is what snapshot 8 sees; 3 shadowed
}

TEST(Compaction, SplitsOutputByTargetSizeWithoutSplittingAKey) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionOptions o = SmallOptions();
  o.target_file_bytes = 2000;
  CompactionManager mgr(vs, o, [] { return 0ull; });  // oldest snapshot 0 => keep ALL versions
  std::uint64_t seq = 1;
  for (int f = 0; f < 4; ++f) {
    std::vector<Entry> entries;
    for (int k = 0; k < 40; ++k) {
      char key[16];
      std::snprintf(key, sizeof key, "key%03d", k);
      entries.push_back(Put(key, seq++, std::string(50, 'x')));
    }
    AddTable(vs, 0, entries);
  }
  auto stats = mgr.RunOnce();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->entries_out, 160u);
  auto v = vs.Current();
  ASSERT_GT(v->NumFiles(1), 3u);
  for (std::size_t i = 1; i < v->levels[1].size(); ++i) {
    // strictly disjoint ranges => no key straddles two files
    EXPECT_LT(v->levels[1][i - 1]->meta.max_key, v->levels[1][i]->meta.min_key);
  }
}

TEST(Compaction, LevelOverflowMovesDataDownAndStaysReadable) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionOptions o = SmallOptions();
  o.level1_max_bytes = 1;
  CompactionManager mgr(vs, o);
  AddTable(vs, 1, {Put("a", 1, "va"), Put("b", 1, "vb")});
  AddTable(vs, 2, {Put("a", 0, "old-a"), Put("c", 0, "vc")});
  ASSERT_TRUE(mgr.RunOnce().has_value());
  auto v = vs.Current();
  EXPECT_EQ(v->NumFiles(1), 0u);
  EXPECT_GE(v->NumFiles(2), 1u);
  EXPECT_EQ(VersionGet(*v, "a")->value, "va");
  EXPECT_EQ(VersionGet(*v, "b")->value, "vb");
  EXPECT_EQ(VersionGet(*v, "c")->value, "vc");
}

// -------------------------------------------------------------- failure paths

TEST(Compaction, CorruptInputAbortsCleanlyAndLeavesVersionUntouched) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  std::vector<std::shared_ptr<FileMeta>> files;
  for (int f = 0; f < 4; ++f) files.push_back(AddTable(vs, 0, {Put("k", 10 + f, "v")}));
  {  // corrupt a byte inside the data section of one input
    std::fstream f(files[2]->meta.path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(18);  // inside the record bytes (offset 17 = key, 18 = value)
    f.put('\x7f');
  }
  auto before = vs.Current();
  EXPECT_THROW(mgr.RunOnce(), CorruptionError);
  EXPECT_EQ(vs.Current(), before);            // same Version object: nothing was published
  EXPECT_EQ(CountSst(dir.path()), 4u);        // no stray outputs
  EXPECT_EQ(CountTmp(dir.path()), 0u);
}

TEST(VersionSetTest, RejectsEditsThatBreakInvariants) {
  TempDir dir;
  VersionSet vs(dir.path());
  AddTable(vs, 1, {Put("a", 1, "v"), Put("m", 1, "v")});
  EXPECT_THROW(AddTable(vs, 1, {Put("k", 1, "v"), Put("z", 1, "v")}), std::logic_error);  // overlaps [a,m]
  EXPECT_EQ(vs.Current()->NumFiles(1), 1u);

  auto stranger = std::make_shared<FileMeta>(999, 0, SSTableMetadata{});
  VersionEdit bad;
  bad.removed.push_back(stranger);
  EXPECT_THROW(vs.Apply(bad), std::logic_error);
}

TEST(VersionSetTest, RecoverRebuildsLevelsFromDirectoryAndRemovesTempFiles) {
  TempDir dir;
  {
    VersionSet vs(dir.path());
    AddTable(vs, 0, {Put("a", 1, "v")});
    AddTable(vs, 1, {Put("b", 2, "v")});
    AddTable(vs, 2, {Put("c", 3, "v")});
    std::ofstream(dir.File("L1-000099.sst.tmp")) << "half written";
  }
  VersionSet recovered(dir.path());
  recovered.Recover();
  auto v = recovered.Current();
  EXPECT_EQ(v->NumFiles(0), 1u);
  EXPECT_EQ(v->NumFiles(1), 1u);
  EXPECT_EQ(v->NumFiles(2), 1u);
  EXPECT_EQ(VersionGet(*v, "c")->value, "v");
  EXPECT_EQ(CountTmp(dir.path()), 0u);
  EXPECT_GT(recovered.NewFileNumber(), 3u);   // file numbers never collide after restart
}

// ----------------------------------------------------------- concurrency

TEST(Compaction, ReadersNeverMissDataWhileCompactionRuns) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  constexpr int kKeys = 200;
  auto key_of = [](int k) { char b[16]; std::snprintf(b, sizeof b, "key%04d", k); return std::string(b); };

  std::atomic<int> errors{0};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> latest_round{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (!stop) {
        std::uint64_t floor = latest_round.load();  // read BEFORE taking the Version
        if (floor == 0) { std::this_thread::yield(); continue; }  // nothing written yet
        auto v = vs.Current();
        for (int k = 0; k < kKeys; k += 7) {
          auto e = VersionGet(*v, key_of(k));
          if (!e || e->type != EntryType::kPut) { ++errors; continue; }
          // value format "r<round>"; the round can never be older than what was complete before we started
          if (std::stoull(e->value.substr(1)) < floor && floor > 0) ++errors;
        }
        std::this_thread::yield();
      }
    });
  }
  std::uint64_t seq = 1;
  for (std::uint64_t round = 1; round <= 12; ++round) {
    std::vector<Entry> entries;
    for (int k = 0; k < kKeys; ++k) entries.push_back(Put(key_of(k), seq++, "r" + std::to_string(round)));
    AddTable(vs, 0, entries);
    latest_round = round;
    while (mgr.RunOnce()) {}
  }
  stop = true;
  for (auto& t : readers) t.join();
  EXPECT_EQ(errors, 0);
  EXPECT_LE(vs.Current()->NumFiles(0), 3u);
  EXPECT_EQ(VersionGet(*vs.Current(), key_of(5))->value, "r12");
}

TEST(CompactionManagerTest, BackgroundWorkerCompactsAfterNotify) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  mgr.Start();
  for (int f = 0; f < 8; ++f) {
    AddTable(vs, 0, {Put("k" + std::to_string(f % 3), 10 + f, "v" + std::to_string(f))});
    mgr.Notify();
  }
  ASSERT_TRUE(mgr.WaitUntilIdle(std::chrono::seconds(10)));
  EXPECT_EQ(mgr.last_error(), "");
  EXPECT_GE(mgr.compactions_completed(), 1u);
  EXPECT_LT(vs.Current()->NumFiles(0), 4u);
  EXPECT_EQ(VersionGet(*vs.Current(), "k1")->value, "v7");   // f=7 -> key k1 (7%3)
  EXPECT_EQ(VersionGet(*vs.Current(), "k0")->value, "v6");
  EXPECT_EQ(VersionGet(*vs.Current(), "k2")->value, "v5");
  mgr.Stop();
  mgr.Stop();  // idempotent
}

TEST(CompactionManagerTest, WorkerReportsErrorAndStaysAlive) {
  TempDir dir;
  VersionSet vs(dir.path());
  CompactionManager mgr(vs, SmallOptions());
  mgr.Start();
  std::vector<std::shared_ptr<FileMeta>> files;
  for (int f = 0; f < 4; ++f) files.push_back(AddTable(vs, 0, {Put("k", 10 + f, "v")}));
  {
    std::fstream f(files[0]->meta.path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(18);  // inside the record bytes (offset 17 = key, 18 = value)
    f.put('\x7f');
  }
  mgr.Notify();
  ASSERT_TRUE(mgr.WaitUntilIdle(std::chrono::seconds(10)));
  EXPECT_NE(mgr.last_error().find("checksum"), std::string::npos);
  EXPECT_EQ(vs.Current()->NumFiles(0), 4u);   // untouched
  mgr.Stop();
}
