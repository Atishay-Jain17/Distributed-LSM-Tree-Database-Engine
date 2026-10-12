#include "test_framework.h"
#include "consensus/raft_log.h"
#include "storage/basic_storage_engine.h"

using namespace lsm;

TEST_CASE(TestRaftLogEmpty) {
    RaftLog log;
    ASSERT_TRUE(log.Empty());
    ASSERT_EQ(log.Size(), 0);
    ASSERT_EQ(log.LastLogIndex(), 0ULL);
    ASSERT_EQ(log.LastLogTerm(), 0ULL);
    ASSERT_EQ(log.CommitIndex(), 0ULL);
    ASSERT_EQ(log.LastApplied(), 0ULL);
    ASSERT_TRUE(log.GetEntry(1) == nullptr);
}

TEST_CASE(TestRaftLogLeaderAppend) {
    RaftLog log;
    uint64_t i1 = log.AppendLeaderEntry(1, "PUT", "k1", "v1");
    uint64_t i2 = log.AppendLeaderEntry(1, "PUT", "k2", "v2");
    uint64_t i3 = log.AppendLeaderEntry(2, "DELETE", "k1", "");

    ASSERT_EQ(i1, 1ULL);
    ASSERT_EQ(i2, 2ULL);
    ASSERT_EQ(i3, 3ULL);
    ASSERT_EQ(log.LastLogIndex(), 3ULL);
    ASSERT_EQ(log.LastLogTerm(), 2ULL);

    const LogEntry* e2 = log.GetEntry(2);
    ASSERT_TRUE(e2 != nullptr);
    ASSERT_EQ(e2->index, 2ULL);
    ASSERT_EQ(e2->term, 1ULL);
    ASSERT_EQ(e2->key, "k2");
    ASSERT_EQ(e2->value, "v2");
}

TEST_CASE(TestRaftLogFollowerConsistency) {
    RaftLog follower_log;
    // Follower has entry (idx: 1, term: 1)
    follower_log.AppendLeaderEntry(1, "PUT", "k1", "v1");

    // Leader sends entry (idx: 2, term: 1) with prev_log_index: 1, prev_log_term: 1
    AppendEntriesArgs args;
    args.term = 1;
    args.leader_id = "leader-1";
    args.prev_log_index = 1;
    args.prev_log_term = 1;
    args.entries = { LogEntry{2, 1, "PUT", "k2", "v2"} };

    AppendEntriesReply reply;
    ASSERT_TRUE(follower_log.HandleAppendEntries(1, args, &reply));
    ASSERT_TRUE(reply.success);
    ASSERT_EQ(reply.match_index, 2ULL);
    ASSERT_EQ(follower_log.LastLogIndex(), 2ULL);

    // Case 2: Follower is behind. Leader tries to append at prev_log_index: 5
    AppendEntriesArgs behind_args;
    behind_args.term = 1;
    behind_args.prev_log_index = 5;
    behind_args.prev_log_term = 1;

    AppendEntriesReply behind_reply;
    ASSERT_FALSE(follower_log.HandleAppendEntries(1, behind_args, &behind_reply));
    ASSERT_FALSE(behind_reply.success);
    ASSERT_EQ(behind_reply.conflict_index, 3ULL); // Next expected index
}

TEST_CASE(TestRaftLogFollowerConflictResolution) {
    RaftLog follower_log;
    // Follower has uncommitted entries from old term: (1, t1), (2, t1), (3, t2)
    follower_log.AppendLeaderEntry(1, "PUT", "k1", "v1");
    follower_log.AppendLeaderEntry(1, "PUT", "k2", "v2");
    follower_log.AppendLeaderEntry(2, "PUT", "k3", "v3_uncommitted");

    ASSERT_EQ(follower_log.LastLogIndex(), 3ULL);

    // New leader in Term 3 sends: prev_log_index: 2, prev_log_term: 1, with entry (3, t3)
    AppendEntriesArgs args;
    args.term = 3;
    args.leader_id = "leader-3";
    args.prev_log_index = 2;
    args.prev_log_term = 1;
    args.entries = { LogEntry{3, 3, "PUT", "k3", "v3_leader_committed"} };

    AppendEntriesReply reply;
    ASSERT_TRUE(follower_log.HandleAppendEntries(3, args, &reply));
    ASSERT_TRUE(reply.success);
    ASSERT_EQ(follower_log.LastLogIndex(), 3ULL);
    ASSERT_EQ(follower_log.LastLogTerm(), 3ULL);

    // Entry 3 should now have term 3 and the leader's value
    const LogEntry* e3 = follower_log.GetEntry(3);
    ASSERT_TRUE(e3 != nullptr);
    ASSERT_EQ(e3->term, 3ULL);
    ASSERT_EQ(e3->value, "v3_leader_committed");
}

TEST_CASE(TestRaftLogPreviousTermMismatchDoesNotMutateFollower) {
    RaftLog follower;
    follower.AppendLeaderEntry(1, "PUT", "k1", "v1");
    follower.AppendLeaderEntry(2, "PUT", "k2", "v2");
    AppendEntriesArgs args;
    args.term = 3;
    args.prev_log_index = 2;
    args.prev_log_term = 99;
    args.entries = {LogEntry{3, 3, "PUT", "k3", "v3"}};
    AppendEntriesReply reply;
    ASSERT_FALSE(follower.HandleAppendEntries(3, args, &reply));
    ASSERT_EQ(follower.LastLogIndex(), 2ULL);
    ASSERT_EQ(follower.TermAt(2), 2ULL);
    ASSERT_EQ(reply.conflict_index, 2ULL);
}

TEST_CASE(TestRaftLogRejectsNonContiguousAppendWithoutMutation) {
    RaftLog follower;
    AppendEntriesArgs args;
    args.term = 1;
    args.entries = {LogEntry{2, 1, "PUT", "k", "v"}};
    AppendEntriesReply reply;
    ASSERT_FALSE(follower.HandleAppendEntries(1, args, &reply));
    ASSERT_TRUE(follower.Empty());
}

TEST_CASE(TestRaftHeartbeatAcknowledgesOnlyMatchedPrefix) {
    RaftLog follower;
    follower.AppendLeaderEntry(1, "PUT", "k1", "v1");
    follower.AppendLeaderEntry(1, "PUT", "k2", "v2");
    AppendEntriesArgs args;
    args.term = 1;
    args.prev_log_index = 1;
    args.prev_log_term = 1;
    args.leader_commit = 1;
    AppendEntriesReply reply;
    ASSERT_TRUE(follower.HandleAppendEntries(1, args, &reply));
    ASSERT_EQ(reply.match_index, 1ULL);
    ASSERT_EQ(follower.CommitIndex(), 1ULL);
}

TEST_CASE(TestRaftAppendCannotReplaceCommittedEntry) {
    RaftLog follower;
    follower.AppendLeaderEntry(1, "PUT", "safe", "value");
    BasicStorageEngine storage;
    ASSERT_STATUS_OK(storage.Open());
    ASSERT_EQ(follower.ApplyCommittedEntries(1, &storage), 1U);
    AppendEntriesArgs args;
    args.term = 2;
    args.entries = {LogEntry{1, 2, "PUT", "unsafe", "replacement"}};
    AppendEntriesReply reply;
    ASSERT_FALSE(follower.HandleAppendEntries(2, args, &reply));
    ASSERT_EQ(follower.GetEntry(1)->key, "safe");
    std::string value;
    ASSERT_STATUS_OK(storage.Get("safe", &value));
    ASSERT_EQ(value, "value");
    storage.Close();
}

TEST_CASE(TestRaftLogOrderedApplicationToStorage) {
    RaftLog log;
    log.AppendLeaderEntry(1, "PUT", "account:1", "100");
    log.AppendLeaderEntry(1, "PUT", "account:2", "200");
    log.AppendLeaderEntry(1, "PUT", "account:1", "150"); // Overwrite
    log.AppendLeaderEntry(2, "DELETE", "account:2", ""); // Delete

    BasicStorageEngine storage;
    ASSERT_STATUS_OK(storage.Open());

    // Initially uncommitted -> nothing applied
    ASSERT_EQ(log.LastApplied(), 0ULL);
    std::string val;
    ASSERT_STATUS_CODE(storage.Get("account:1", &val), StatusCode::NotFound);

    // Advance commit index to 2
    size_t applied = log.ApplyCommittedEntries(2, &storage);
    ASSERT_EQ(applied, 2);
    ASSERT_EQ(log.LastApplied(), 2ULL);
    ASSERT_EQ(log.CommitIndex(), 2ULL);

    ASSERT_STATUS_OK(storage.Get("account:1", &val));
    ASSERT_EQ(val, "100");
    ASSERT_STATUS_OK(storage.Get("account:2", &val));
    ASSERT_EQ(val, "200");

    // Commit remainder up to index 4
    applied = log.ApplyCommittedEntries(4, &storage);
    ASSERT_EQ(applied, 2);
    ASSERT_EQ(log.LastApplied(), 4ULL);
    ASSERT_EQ(log.CommitIndex(), 4ULL);

    // Verify final state: account:1 is 150, account:2 is deleted
    ASSERT_STATUS_OK(storage.Get("account:1", &val));
    ASSERT_EQ(val, "150");
    ASSERT_STATUS_CODE(storage.Get("account:2", &val), StatusCode::NotFound);
    ASSERT_EQ(storage.EntryCount(), 1);

    storage.Close();
}
