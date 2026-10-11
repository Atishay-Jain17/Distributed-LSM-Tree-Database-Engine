# Distributed write path (Week 6)

```
client -> ClusterClient -> any node's KvService
   leader:   ReplicatedWriteHandler -> RaftProposer.Propose (blocks until quorum commit)
             -> commit callback -> OrderedApplier -> storage -> OK to client
   follower: Propose returns NOT_LEADER + leader_hint -> client jumps to the leader
   no quorum: UNAVAILABLE, nothing applied
```

**Contract with Raft (Atishay / Sarthak): `consensus/raft_proposer.h`.** Implement `Propose(payload, timeout)`
(leader: append + replicate + block until committed, returns the log index; follower: NOT_LEADER with hint) and
`SetCommitCallback(cb)` (call `cb(index, payload)` for every committed entry on every node). Nothing else in my
code depends on Raft internals. `tests/support/fake_raft.h` is an in-process stand-in; run the same
`tests/consensus` tests against the real Raft by swapping the proposer.

**Log payload:** `ReplicatedCommand` (`proto/lsmdb/v1/command.proto`): PUT or DELETE, key, value. Raft treats it as bytes.

**Ordering guarantee:** `OrderedApplier` applies strictly by log index, once each, tolerating reordered or
repeated delivery. Same log + same order => identical replicas. If storage throws, applying stops and the error
is reported instead of letting the replica diverge.

**Client semantics:** OK means committed by a quorum and applied on the node that served the request. UNAVAILABLE
is retryable (Put/Delete are idempotent). Reads are local, so a follower can briefly lag the leader.

**Sequence numbers:** the Raft log index is the MVCC `seq` for MemTable/SSTable entries, so versions are globally ordered.

**Not in scope / assumptions:** leader election, log replication, quorum counting and snapshots belong to the Raft
module; read routing / read-your-writes on followers belongs to Suhani's Week 6 read path.
