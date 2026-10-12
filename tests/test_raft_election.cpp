#include "test_framework.h"
#include "consensus/raft_state.h"
#include <filesystem>
#include <fstream>

using namespace lsm;

TEST_CASE(TestRaftStateInitialization) {
    RaftStateMachine node("node-1", {"node-2", "node-3"}, "");
    ASSERT_STATUS_OK(node.Init());

    ASSERT_EQ(node.Role(), RaftRole::Follower);
    ASSERT_EQ(node.CurrentTerm(), 0ULL);
    ASSERT_EQ(node.VotedFor(), "");
    ASSERT_FALSE(node.IsLeader());
}

TEST_CASE(TestRaftStatePersistence) {
    std::string state_dir = "./test_raft_state_dir";
    std::filesystem::remove_all(state_dir);

    {
        RaftStateMachine node("node-1", {"node-2", "node-3"}, state_dir);
        ASSERT_STATUS_OK(node.Init());
        ASSERT_EQ(node.CurrentTerm(), 0ULL);

        // Start election -> term should increment to 1, voted_for becomes "node-1"
        RequestVoteArgs args = node.StartElection();
        ASSERT_EQ(args.term, 1ULL);
        ASSERT_EQ(args.candidate_id, "node-1");
        ASSERT_EQ(node.CurrentTerm(), 1ULL);
        ASSERT_EQ(node.VotedFor(), "node-1");

        // Replacing an existing state file must preserve a complete new state.
        const RequestVoteArgs next = node.StartElection();
        ASSERT_EQ(next.term, 2ULL);
    }

    // Reopen node from persistent directory and check state was restored
    {
        RaftStateMachine node_recovered("node-1", {"node-2", "node-3"}, state_dir);
        ASSERT_STATUS_OK(node_recovered.Init());
        ASSERT_EQ(node_recovered.CurrentTerm(), 2ULL);
        ASSERT_EQ(node_recovered.VotedFor(), "node-1");
        ASSERT_EQ(node_recovered.Role(), RaftRole::Follower);
    }

    std::filesystem::remove_all(state_dir);
}

TEST_CASE(TestRaftPersistentStateRejectsTruncatedFile) {
    const std::string dir = "./test_raft_state_corrupt";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    { std::ofstream out(dir + "/raft_state.bin", std::ios::binary); out.put('\x01'); }
    RaftStateMachine node("node-1", {}, dir);
    const Status status = node.Init();
    ASSERT_STATUS_CODE(status, StatusCode::Corruption);
    std::filesystem::remove_all(dir);
}

TEST_CASE(TestRaftRequestVoteRules) {
    RaftStateMachine follower("node-2", {"node-1", "node-3"}, "");
    ASSERT_STATUS_OK(follower.Init());

    // 1. Stale term rejected
    RequestVoteArgs stale_args{0, "node-1", 0, 0};
    RequestVoteReply reply;
    follower.HandleRequestVote(stale_args, &reply);
    // Term is 0, so candidate term 0 is not < current_term.
    // Vote is granted for term 0:
    ASSERT_TRUE(reply.vote_granted);
    ASSERT_EQ(follower.VotedFor(), "node-1");

    // 2. Reject second candidate in the same term
    RequestVoteArgs conflict_args{0, "node-3", 0, 0};
    follower.HandleRequestVote(conflict_args, &reply);
    ASSERT_FALSE(reply.vote_granted);
    ASSERT_EQ(follower.VotedFor(), "node-1");

    // 3. Stale term (< current_term) rejected when follower term advances
    follower.StepDown(5);
    RequestVoteArgs old_args{3, "node-3", 0, 0};
    follower.HandleRequestVote(old_args, &reply);
    ASSERT_FALSE(reply.vote_granted);
    ASSERT_EQ(reply.term, 5ULL);

    // 4. Grant vote when higher term arrives
    RequestVoteArgs higher_args{6, "node-3", 0, 0};
    follower.HandleRequestVote(higher_args, &reply);
    ASSERT_TRUE(reply.vote_granted);
    ASSERT_EQ(follower.CurrentTerm(), 6ULL);
    ASSERT_EQ(follower.VotedFor(), "node-3");
}

TEST_CASE(TestRaftSimulatedThreeNodeElection) {
    RaftStateMachine node1("node-1", {"node-2", "node-3"}, "");
    RaftStateMachine node2("node-2", {"node-1", "node-3"}, "");
    RaftStateMachine node3("node-3", {"node-1", "node-2"}, "");

    ASSERT_STATUS_OK(node1.Init());
    ASSERT_STATUS_OK(node2.Init());
    ASSERT_STATUS_OK(node3.Init());

    // Node 1 times out and triggers election
    RequestVoteArgs args = node1.StartElection();
    ASSERT_EQ(node1.Role(), RaftRole::Candidate);
    ASSERT_EQ(node1.CurrentTerm(), 1ULL);

    // Send RequestVote to Node 2 and Node 3
    RequestVoteReply reply2;
    node2.HandleRequestVote(args, &reply2);
    ASSERT_TRUE(reply2.vote_granted);

    RequestVoteReply reply3;
    node3.HandleRequestVote(args, &reply3);
    ASSERT_TRUE(reply3.vote_granted);

    // Process reply from Node 2 (Node 1 has self vote + Node 2 vote = 2 out of 3 = majority!)
    bool became_leader = node1.HandleVoteReply("node-2", reply2);
    ASSERT_TRUE(became_leader);
    ASSERT_FALSE(node1.HandleVoteReply("node-2", reply2)); // Retransmitted vote is not counted twice.
    ASSERT_TRUE(node1.IsLeader());
    ASSERT_EQ(node1.Role(), RaftRole::Leader);


    // Node 3 starts an election in higher term (term 2)
    RequestVoteArgs args3 = node3.StartElection();
    ASSERT_EQ(args3.term, 2ULL);

    // Node 1 receives RequestVote from Node 3 -> Node 1 must step down from Leader!
    RequestVoteReply reply1;
    node1.HandleRequestVote(args3, &reply1);
    ASSERT_TRUE(reply1.vote_granted);
    ASSERT_FALSE(node1.IsLeader());
    ASSERT_EQ(node1.Role(), RaftRole::Follower);
    ASSERT_EQ(node1.CurrentTerm(), 2ULL);
}

TEST_CASE(TestRaftVoteReplyRejectsUnknownPeer) {
    RaftStateMachine node("node-1", {"node-2", "node-3"}, "");
    ASSERT_STATUS_OK(node.Init());
    const auto args = node.StartElection();
    RequestVoteReply granted{args.term, true};
    ASSERT_FALSE(node.HandleVoteReply("outsider", granted));
    ASSERT_FALSE(node.IsLeader());
}

TEST_CASE(TestRaftRandomElectionTimeoutRange) {
    RaftStateMachine node("node-1", {"node-2", "node-3"}, "", 150, 300);
    for (int i = 0; i < 50; ++i) {
        auto timeout = node.GetRandomElectionTimeout();
        ASSERT_TRUE(timeout.count() >= 150);
        ASSERT_TRUE(timeout.count() <= 300);
    }
}
