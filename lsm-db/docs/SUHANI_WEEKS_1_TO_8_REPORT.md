# Suhani Weeks 1–8 delivery status

This report follows `Distributed_LSM_Tree_8_Week_Work_Report_FINAL(1).pdf`. It records what is implemented in this checkout and separates local prototype behavior from work that needs the other owners' distributed components.

## Implemented in this checkout

| Week | Suhani scope | Project state |
|---|---|---|
| 1 | Kubernetes/Helm foundation, stable node identity, PVCs and configuration | Helm chart, StatefulSet, headless/client Services, persistent-volume templates, per-pod node ID and peer environment, Dockerfile, and deployment instructions are present. Not run against a live cluster here. |
| 2 | Public storage API and single-node integration | `api::MemTableBackend` implements PUT/GET/DELETE with monotonically increasing versions, tombstones, and snapshot reads. The combined root build now uses `AtishaySkipListMemTable`, a binary-safe and locked adapter over Atishay's SkipList. |
| 3 | Startup recovery | WAL replay is called by `PersistentBackend::Open` before the RPC server starts. The combined build adapts Atishay's WAL; incomplete tail bytes are truncated before append, while complete checksum failures and interior corruption fail startup. WAL appends sync before MemTable mutation, and flush checkpoints remove records already covered by durable tables while preserving newer records. |
| 4 | Rotation, flush, compaction scheduling and file lifecycle | `LsmStore` owns rotation/workers/snapshots. On Unix the node now uses `AtishaySSTableTableStore`, which writes checksummed sparse-index/Bloom SSTables, validates them at startup, persists manifests and sequence high-water across compaction, and reclaims obsolete files after readers release them. Windows retains `ReferenceTableStore`. A conditional storage integration test covers flush/read/compaction/restart. |
| 5 | Replicated command and Raft/storage boundary | Versioned command codec, strict validation, state-machine apply, persistent `RaftNode`, gRPC RequestVote/AppendEntries, and `NOT_LEADER`/leader endpoint mapping are implemented in source. |
| 6 | Leader-based reads | The CLI node uses a quorum-proposed backend. A per-term no-op establishes the current-term commit; reads require quorum heartbeat confirmation and wait for local apply. The three-node test checks leader read success, follower read rejection, quorum writes, and reads after re-election. The RPC/Raft source has not been compiled or executed here because the required gRPC/Protobuf toolchain is absent. |
| 7 | Pod crash and network partition scenarios | Chaos Mesh `PodChaos` and `NetworkChaos` manifests, a Raft-aware three-node CLI smoke script, a PVC restart/recovery script, and an evidence template are present. The local gRPC integration test now restarts persistent nodes, tests catch-up, denies commit without a majority, and checks re-election. No live Kubernetes/Chaos Mesh run was possible in this workspace. |
| 8 | Deployment, evidence, documentation and final smoke | Docker/Helm packaging, CLI operations with leader redirects, node Raft status inspection, deployment scripts, and evidence structure are present. Clean deployment and cross-node failure evidence still require a cluster run. |

## Combined build and verification

From the repository root, the Suhani core libraries and dependency-free persistent-node smoke test are part of the default CMake build. RPC and the imported GoogleTest suites are optional because they require Protobuf/gRPC and GoogleTest packages:

```powershell
cmake -S . -B build/verify -G "MinGW Makefiles"
cmake --build build/verify
ctest --test-dir build/verify --output-on-failure
```

The imported subproject can build its core independently with:

```powershell
cmake -S lsm-db -B build/suhani-core -DLSMDB_BUILD_RPC=OFF -DLSMDB_BUILD_TESTS=OFF
cmake --build build/suhani-core
ctest --test-dir build/suhani-core --output-on-failure
```

To build the RPC node and CLI, install Protobuf, gRPC C++ development files, `protoc`, `grpc_cpp_plugin`, and `pkg-config`, then configure with `-DLSMDB_BUILD_SUHANI_RPC=ON`. To run the full imported test suites, install GoogleTest and set `-DLSMDB_BUILD_SUHANI_TESTS=ON`.

## Integration boundaries and limits

- `PersistentBackend` integrates the Suhani API, recovery, and lifecycle with the gRPC node. In the combined root build it uses Atishay's SkipList through `AtishaySkipListMemTable`, Atishay's WAL through an adapter, and on Unix Atishay's indexed/Bloom SSTables through `AtishaySSTableTableStore`. The portable reference table format remains the Windows fallback.
- The Raft peer protocol and proposer are implemented in source, but have not been compiled or exercised in this environment because Protobuf/gRPC are unavailable. Do not treat source presence or the new integration test as runtime proof.
- The Unix SSTable adapter and its integration test are source-present but have not been built or executed here because this workstation has no Linux/WSL runtime. The Windows smoke test exercises the reference fallback.
- The deployment smoke script checks for one leader, exercises follower redirects, and verifies all replicas applied the same committed index. It and the Chaos Mesh scenarios have not been executed here; the evidence report remains intentionally blank.
- The gRPC and complete Suhani GoogleTest suites could not be built in this Windows environment: Protobuf/gRPC `pkg-config` metadata and GoogleTest libraries are not installed. The top-level build's four available CTest groups (including persistent-node and Bloom-filter smoke groups) were run successfully after integration.
- The Docker builder is configured to compile the full Linux/gRPC and GoogleTest targets and run CTest before packaging the runtime image. Docker is unavailable here, so that Linux validation gate remains unexecuted and its result is not claimed.

## Run the deployment prototype

```bash
docker build -f lsm-db/deploy/Dockerfile -t lsmdb:local .
helm upgrade --install lsmdb lsm-db/deploy/helm/lsmdb --set image.repository=lsmdb --set image.tag=local
bash lsm-db/deploy/scripts/deployment-smoke.sh lsmdb default
```

The cluster smoke verifies Raft behavior when run in a Kubernetes environment. The PVC script verifies local WAL/table recovery for one pod. Neither script has been executed in this workstation session.
