#pragma once

namespace lsmdb::reads {

// Read consistency guarantees for distributed queries.
enum class ConsistencyLevel {
  // Linearizable reads:
  // - Served exclusively by the verified leader.
  // - The leader verifies it still holds a valid quorum lease or checks quorum before serving.
  // - Followers reject reads on the normal client path with STATUS_CODE_NOT_LEADER + leader_hint.
  // - Guarantees monotonic reads and read-your-writes across all clients.
  kLinearizable = 1,

  // Sequential consistency:
  // - Served by the leader directly from its applied storage state without re-verifying quorum lease.
  // - Still strictly serialized with respect to the leader's commit log.
  kSequential = 2,

  // Stale-tolerant (local) reads:
  // - Any replica serves directly from its local storage.
  // - May return data lagging behind the leader's committed state.
  kStaleTolerant = 3,
};

}  // namespace lsmdb::reads
