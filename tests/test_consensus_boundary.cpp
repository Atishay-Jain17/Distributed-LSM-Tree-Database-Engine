#include "test_framework.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "lsmdb/api/memtable_backend.h"
#include "lsmdb/api/reference_memtable.h"
#include "lsmdb/consensus/boundary.h"
#include "lsmdb/consensus/command.h"
#include "lsmdb/consensus/reference_consensus.h"
#include "lsmdb/consensus/replicated_backend.h"
#include "lsmdb/consensus/storage_state_machine.h"

using namespace lsmdb;
using namespace lsmdb::consensus;

TEST_CASE(TestStorageStateMachineAppliesCommandsInOrder) {
    auto storage = std::make_shared<api::MemTableBackend>(std::make_shared<api::ReferenceMemTable>());
    StorageStateMachine sm(storage);

    ASSERT_TRUE(sm.Apply(1, Encode(Command::Put("user:1", "Alice"))) == ApplyOutcome::kApplied);
    ASSERT_TRUE(sm.Apply(2, Encode(Command::Put("user:2", "Bob"))) == ApplyOutcome::kApplied);
    ASSERT_TRUE(sm.Apply(3, Encode(Command::Delete("user:1"))) == ApplyOutcome::kApplied);
    ASSERT_TRUE(sm.Apply(4, Encode(Command::Noop())) == ApplyOutcome::kApplied);

    ASSERT_EQ(sm.LastApplied(), 4ULL);
    ASSERT_FALSE(storage->Get("user:1").has_value());
    ASSERT_TRUE(storage->Get("user:2").has_value());
    ASSERT_EQ(*storage->Get("user:2"), "Bob");

    // Duplicate apply must be recognized and skipped
    ASSERT_TRUE(sm.Apply(3, Encode(Command::Delete("user:1"))) == ApplyOutcome::kSkippedDuplicate);
}

TEST_CASE(TestReplicatedBackendPutGetDeleteAndReadBarrier) {
    auto storage = std::make_shared<api::MemTableBackend>(std::make_shared<api::ReferenceMemTable>());
    auto sm = std::make_shared<StorageStateMachine>(storage);
    auto consensus = std::make_shared<ReferenceConsensus>(sm);
    ReplicatedBackend backend(consensus, storage);

    backend.Put("item:100", "Widget");
    auto res = backend.Get("item:100");
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(*res, "Widget");

    backend.Delete("item:100");
    res = backend.Get("item:100");
    ASSERT_FALSE(res.has_value());

    // Follower rejection and leader hint test
    consensus->SetLeader(false, 3);
    bool caught_not_leader = false;
    try {
        backend.Put("item:101", "Gadget");
    } catch (const NotLeaderError& err) {
        caught_not_leader = true;
        ASSERT_EQ(err.leader_hint, 3U);
    }
    ASSERT_TRUE(caught_not_leader);

    // Read barrier rejection on follower
    bool caught_follower_read = false;
    try {
        (void)backend.Get("item:100");
    } catch (const NotLeaderError& err) {
        caught_follower_read = true;
        ASSERT_EQ(err.leader_hint, 3U);
    }
    ASSERT_TRUE(caught_follower_read);
}
