// Raft/storage boundary tests (Week 5): state machine, stand-in consensus, replicated backend.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include <unistd.h>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/consensus_boundary/reference_consensus.h"
#include "lsmdb/consensus_boundary/replicated_backend.h"
#include "lsmdb/consensus_boundary/storage_state_machine.h"
#include "lsmdb/lifecycle/lsm_store.h"
#include "lsmdb/lifecycle/reference_table_store.h"

using namespace lsmdb;
using namespace lsmdb::consensus_boundary;
using std::chrono::milliseconds;

namespace {

std::shared_ptr<api::MemTableBackend> NewStorage() {
  return std::make_shared<api::MemTableBackend>(std::make_shared<api::ReferenceMemTable>());
}

class CountingStorage : public rpc::StorageBackend {
 public:
  void Put(const std::string& k, const std::string& v) override { ++writes; data[k] = v; }
  std::optional<std::string> Get(const std::string& k) override {
    auto it = data.find(k);
    if (it == data.end()) return std::nullopt;
    return it->second;
  }
  void Delete(const std::string& k) override { ++writes; data.erase(k); }
  int writes = 0;
  std::map<std::string, std::string> data;
};

class FailingStorage : public rpc::StorageBackend {
 public:
  void Put(const std::string&, const std::string&) override { throw std::runtime_error("disk full"); }
  std::optional<std::string> Get(const std::string&) override { return std::nullopt; }
  void Delete(const std::string&) override { throw std::runtime_error("disk full"); }
};

}  // namespace

// ---- StorageStateMachine ---------------------------------------------------------------------

TEST(StateMachine, AppliesPutAndDeleteInOrder) {
  auto st = NewStorage();
  StorageStateMachine sm(st);
  EXPECT_EQ(sm.Apply(1, Encode(Command::Put("a", "1"))), ApplyOutcome::kApplied);
  EXPECT_EQ(sm.Apply(2, Encode(Command::Put("a", "2"))), ApplyOutcome::kApplied);
  EXPECT_EQ(sm.Apply(3, Encode(Command::Delete("a"))), ApplyOutcome::kApplied);
  EXPECT_EQ(sm.Apply(4, Encode(Command::Put("b", "x"))), ApplyOutcome::kApplied);
  EXPECT_FALSE(st->Get("a").has_value());
  EXPECT_EQ(st->Get("b"), std::optional<std::string>("x"));
  EXPECT_EQ(sm.LastApplied(), 4u);
}

TEST(StateMachine, DuplicateIndexIsSkippedAndDoesNotTouchStorage) {
  auto st = std::make_shared<CountingStorage>();
  StorageStateMachine sm(st);
  ASSERT_EQ(sm.Apply(1, Encode(Command::Put("a", "1"))), ApplyOutcome::kApplied);
  EXPECT_EQ(sm.Apply(1, Encode(Command::Put("a", "OTHER"))), ApplyOutcome::kSkippedDuplicate);
  EXPECT_EQ(st->writes, 1);
  EXPECT_EQ(st->data["a"], "1");
}

TEST(StateMachine, GapInIndexesIsAnError) {
  StorageStateMachine sm(NewStorage());
  EXPECT_THROW(sm.Apply(2, Encode(Command::Put("a", "1"))), std::logic_error);
  EXPECT_EQ(sm.LastApplied(), 0u);
}

TEST(StateMachine, StartsAfterRestoredLastApplied) {
  auto st = std::make_shared<CountingStorage>();
  StorageStateMachine sm(st, 10);
  EXPECT_EQ(sm.Apply(10, Encode(Command::Put("a", "1"))), ApplyOutcome::kSkippedDuplicate);
  EXPECT_EQ(sm.Apply(11, Encode(Command::Put("a", "1"))), ApplyOutcome::kApplied);
}

TEST(StateMachine, MalformedAndInvalidEntriesAreRejectedBeforeStorage) {
  auto st = std::make_shared<CountingStorage>();
  StorageStateMachine sm(st);
  EXPECT_EQ(sm.Apply(1, "garbage"), ApplyOutcome::kRejectedMalformed);
  Command bad = Command::Put("", "v");  // empty key
  EXPECT_EQ(sm.Apply(2, Encode(bad)), ApplyOutcome::kRejectedInvalid);
  Command big = Command::Put("k", std::string(kMaxValueBytes + 1, 'x'));
  EXPECT_EQ(sm.Apply(3, Encode(big)), ApplyOutcome::kRejectedInvalid);
  EXPECT_EQ(st->writes, 0);
  EXPECT_EQ(sm.rejected(), 3u);
  EXPECT_EQ(sm.LastApplied(), 3u);   // rejected entries are consumed, so replicas stay in step
  EXPECT_EQ(sm.Apply(4, Encode(Command::Put("ok", "1"))), ApplyOutcome::kApplied);
}

TEST(StateMachine, StorageFailureDoesNotAdvanceLastApplied) {
  StorageStateMachine sm(std::make_shared<FailingStorage>());
  EXPECT_THROW(sm.Apply(1, Encode(Command::Put("a", "1"))), std::runtime_error);
  EXPECT_EQ(sm.LastApplied(), 0u);
}

// ---- ReferenceConsensus: proposals ------------------------------------------------------------

TEST(Consensus, ProposedEntryIsNotVisibleUntilCommitted) {
  auto st = NewStorage();
  auto sm = std::make_shared<StorageStateMachine>(st);
  ReferenceConsensus raft(sm, {.auto_commit = false});
  ProposeResult r = raft.Propose(Command::Put("k", "v"));
  ASSERT_EQ(r.status, ProposeStatus::kAccepted);
  EXPECT_EQ(r.index, 1u);
  EXPECT_EQ(raft.LastIndex(), 1u);
  EXPECT_EQ(raft.CommitIndex(), 0u);
  EXPECT_FALSE(st->Get("k").has_value());      // proposed, not committed: storage untouched
  EXPECT_FALSE(raft.WaitApplied(1, milliseconds(20)));
  ASSERT_TRUE(raft.CommitAll());
  EXPECT_EQ(st->Get("k"), std::optional<std::string>("v"));
  EXPECT_TRUE(raft.WaitApplied(1, milliseconds(20)));
}

TEST(Consensus, LogHoldsEncodedCommandsThatDecodeToWhatWasProposed) {
  ReferenceConsensus raft(std::make_shared<StorageStateMachine>(NewStorage()), {.auto_commit = false});
  std::vector<Command> sent = {Command::Put("a", "1", "r1"), Command::Delete("a", "r2"),
                               Command::Put(std::string("b\0c", 3), std::string("\xff\0", 2), "")};
  for (const auto& c : sent) ASSERT_EQ(raft.Propose(c).status, ProposeStatus::kAccepted);
  auto log = raft.LogSnapshot();
  ASSERT_EQ(log.size(), sent.size());
  for (std::size_t i = 0; i < sent.size(); ++i) {
    DecodeResult d = Decode(log[i]);
    ASSERT_TRUE(d);
        EXPECT_EQ(d.command->type, sent[i].type);
    EXPECT_EQ(d.command->key, sent[i].key);
    EXPECT_EQ(d.command->value, sent[i].value);
  }
}

TEST(Consensus, InvalidCommandIsRefusedAndNeverLogged) {
  ReferenceConsensus raft(std::make_shared<StorageStateMachine>(NewStorage()));
  ProposeResult r = raft.Propose(Command::Put("", "v"));
  EXPECT_EQ(r.status, ProposeStatus::kInvalid);
  EXPECT_FALSE(r.message.empty());
  EXPECT_EQ(raft.LastIndex(), 0u);
}

TEST(Consensus, CommitAppliesInLogOrder) {
  auto st = std::make_shared<CountingStorage>();
  ReferenceConsensus raft(std::make_shared<StorageStateMachine>(st), {.auto_commit = false});
  raft.Propose(Command::Put("k", "1"));
  raft.Propose(Command::Put("k", "2"));
  raft.Propose(Command::Put("k", "3"));
  ASSERT_TRUE(raft.CommitUpTo(2));
  EXPECT_EQ(st->data["k"], "2");
  EXPECT_EQ(raft.CommitIndex(), 2u);
  ASSERT_TRUE(raft.CommitAll());
  EXPECT_EQ(st->data["k"], "3");
}

TEST(Consensus, NotLeaderRefusesWithHint) {
  ReferenceConsensus raft(std::make_shared<StorageStateMachine>(NewStorage()),
                          {.is_leader = false, .leader_hint = 3});
  ProposeResult r = raft.Propose(Command::Put("k", "v"));
  EXPECT_EQ(r.status, ProposeStatus::kNotLeader);
  EXPECT_EQ(r.leader_hint, 3u);
  EXPECT_EQ(raft.LastIndex(), 0u);
  raft.SetLeader(true);
  EXPECT_EQ(raft.Propose(Command::Put("k", "v")).status, ProposeStatus::kAccepted);
}

TEST(Consensus, ApplyFailureStopsConsensusAndKeepsEntryUncommitted) {
  ReferenceConsensus raft(std::make_shared<StorageStateMachine>(std::make_shared<FailingStorage>()));
  ProposeResult r = raft.Propose(Command::Put("k", "v"));
  EXPECT_EQ(r.status, ProposeStatus::kStopped);
  EXPECT_TRUE(raft.stopped());
  EXPECT_EQ(raft.CommitIndex(), 0u);
  EXPECT_EQ(raft.Propose(Command::Put("k2", "v")).status, ProposeStatus::kStopped);
}

TEST(Consensus, ContinuesAfterAlreadyAppliedIndexes) {
  auto st = std::make_shared<CountingStorage>();
  ReferenceConsensus raft(std::make_shared<StorageStateMachine>(st, 5));
  ProposeResult r = raft.Propose(Command::Put("k", "v"));
  ASSERT_EQ(r.status, ProposeStatus::kAccepted);
  EXPECT_EQ(r.index, 6u);
  EXPECT_EQ(st->data["k"], "v");
}

TEST(Consensus, ConcurrentProposalsGetUniqueIndexesAndAreAppliedOnce) {
  auto st = NewStorage();
  auto sm = std::make_shared<StorageStateMachine>(st);
  ReferenceConsensus raft(sm);
  constexpr int kThreads = 8, kPer = 200;
  std::vector<std::thread> ts;
  std::vector<std::vector<LogIndex>> idx(kThreads);
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) {
        ProposeResult r = raft.Propose(Command::Put("t" + std::to_string(t) + "k" + std::to_string(i), "v"));
        ASSERT_EQ(r.status, ProposeStatus::kAccepted);
        idx[t].push_back(r.index);
      }
    });
  for (auto& th : ts) th.join();
  std::set<LogIndex> all;
  for (auto& v : idx) all.insert(v.begin(), v.end());
  EXPECT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPer));
  EXPECT_EQ(*all.begin(), 1u);
  EXPECT_EQ(*all.rbegin(), static_cast<LogIndex>(kThreads * kPer));
  EXPECT_EQ(sm->LastApplied(), static_cast<LogIndex>(kThreads * kPer));
  for (int t = 0; t < kThreads; ++t)
    for (int i = 0; i < kPer; ++i)
      ASSERT_TRUE(st->Get("t" + std::to_string(t) + "k" + std::to_string(i)).has_value());
}

// ---- ReplicatedBackend: a command through the whole boundary ----------------------------------

TEST(ReplicatedBackend, PutGetDeleteGoThroughTheLog) {
  auto st = NewStorage();
  auto raft = std::make_shared<ReferenceConsensus>(std::make_shared<StorageStateMachine>(st));
  ReplicatedBackend db(raft, st);
  db.Put("k", "v1");
  EXPECT_EQ(db.Get("k"), std::optional<std::string>("v1"));
  db.Put("k", "v2");
  db.Delete("k");
  db.Delete("never-existed");
  EXPECT_FALSE(db.Get("k").has_value());
  EXPECT_EQ(raft->LastIndex(), 4u);   // every write is one log entry, deletes included
  EXPECT_EQ(raft->CommitIndex(), 4u);
}

TEST(ReplicatedBackend, InvalidWriteThrowsInvalidArgumentAndIsNotLogged) {
  auto st = NewStorage();
  auto raft = std::make_shared<ReferenceConsensus>(std::make_shared<StorageStateMachine>(st));
  ReplicatedBackend db(raft, st);
  EXPECT_THROW(db.Put("", "v"), std::invalid_argument);
  EXPECT_EQ(raft->LastIndex(), 0u);
}

TEST(ReplicatedBackend, FollowerThrowsNotLeaderWithHint) {
  auto st = NewStorage();
  auto raft = std::make_shared<ReferenceConsensus>(std::make_shared<StorageStateMachine>(st),
                                                   ReferenceConsensusOptions{.is_leader = false, .leader_hint = 2});
  ReplicatedBackend db(raft, st);
  try {
    db.Put("k", "v");
    FAIL() << "expected NotLeaderError";
  } catch (const NotLeaderError& e) {
    EXPECT_EQ(e.leader_hint, 2u);
  }
  EXPECT_FALSE(db.Get("k").has_value());
}

TEST(ReplicatedBackend, WriteThatIsProposedButNeverCommittedTimesOut) {
  auto st = NewStorage();
  auto raft = std::make_shared<ReferenceConsensus>(std::make_shared<StorageStateMachine>(st),
                                                   ReferenceConsensusOptions{.auto_commit = false});
  ReplicatedBackend db(raft, st, milliseconds(50));
  EXPECT_THROW(db.Put("k", "v"), std::runtime_error);
  EXPECT_FALSE(db.Get("k").has_value());      // not committed, so not visible
  raft->CommitAll();
  EXPECT_EQ(db.Get("k"), std::optional<std::string>("v"));
}

TEST(ReplicatedBackend, WorksOnTopOfTheFlushingLsmStore) {
  // Same boundary, but storage is the Week 4 store with a tiny MemTable so flushes and
  // compactions happen while commands are being applied.
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / ("lsmdb_week5_lsm_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  {
    lifecycle::LsmOptions opt;
    opt.memtable_bytes = 512;
    auto store = std::make_shared<lifecycle::LsmStore>(
        std::make_shared<lifecycle::ReferenceTableStore>(dir, lifecycle::ReferenceTableStoreOptions{.compaction_trigger = 3}), opt);
    auto raft = std::make_shared<ReferenceConsensus>(std::make_shared<StorageStateMachine>(store));
    ReplicatedBackend db(raft, store);
    for (int i = 0; i < 300; ++i) db.Put("key" + std::to_string(i % 40), "value" + std::to_string(i));
    for (int i = 0; i < 40; i += 2) db.Delete("key" + std::to_string(i));
    store->Flush();
    for (int i = 0; i < 40; ++i) {
      auto got = db.Get("key" + std::to_string(i));
      if (i % 2 == 0) EXPECT_FALSE(got.has_value()) << i;
      else ASSERT_TRUE(got.has_value()) << i;
    }
    EXPECT_GE(store->stats().flushes, 1u);
  }
  fs::remove_all(dir);
}
