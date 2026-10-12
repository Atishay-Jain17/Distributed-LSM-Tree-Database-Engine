# Week 1 — Kubernetes/Helm Infrastructure (Suhani)

## What's here

```
deploy/
├── placeholder-node/          # Minimal C++ binary + Dockerfile (temporary stand-in
│   ├── src/main.cpp           # for the real node, until the gRPC service exists)
│   ├── CMakeLists.txt
│   └── Dockerfile
├── helm/lsm-db/
│   ├── Chart.yaml
│   ├── values.yaml            # All tunables: replica count, image, ports, storage
│   └── templates/
│       ├── statefulset.yaml       # 3-node StatefulSet, stable identity, peer list
│       └── service-headless.yaml  # Headless service for per-pod DNS discovery
├── tests/
│   └── lifecycle-test.sh      # Pod restart / rescheduling / PV reattachment test
├── smoke-test.sh              # Build + deploy + verify the baseline cluster
└── README.md
```

## How this maps to Week 1's task list

| Day | Task | Where |
|---|---|---|
| Fri | Kubernetes baseline | `helm/lsm-db/` chart + `statefulset.yaml` |
| Sat | Stable identity | `service-headless.yaml` + StatefulSet naming |
| Sun | Cluster smoke test + commit | `smoke-test.sh` |
| Mon | Persistent storage | `volumeClaimTemplates` in the StatefulSet, tuned in `values.yaml` |
| Tue | Configuration injection | start-up script in the StatefulSet that turns the pod name into the `LSMDB_*` variables the real node reads (see "Configuration contract" below) |
| Wed | Lifecycle tests | `tests/lifecycle-test.sh` |
| Thu | Deployment handoff | This README |

## Architecture decisions and why

- **StatefulSet, not Deployment**: gives each pod a permanent name (`lsm-db-0/1/2`) and its own PersistentVolumeClaim that survive restarts and rescheduling — required for Raft, where node identity must stay stable.
- **Headless Service**: skips load-balancing so each pod gets its own individually-addressable DNS name, which peer-to-peer consensus protocols need (a normal Service would hide which specific node you're talking to).
- **Config derived inside the pod**: Helm cannot know which pod it is rendering for, so a small shell script in the container command computes the node ID (pod number + 1) and the peer list (every other pod, as `id=host:port`) from the pod name and `replicaCount`. One image and one template serve all replicas, and the values match what `lsmdb_node` expects (`lsm-db/docs/rpc-contract.md`).
- **Storage class**: defaults to `standard` (kind's local-path-provisioner) for local development. This must be changed to a real cloud provider's storage class before this chart is used outside of local `kind` testing — noted explicitly in `values.yaml`.

## Configuration contract for the real node

The StatefulSet start-up script sets these before starting the node (shown for pod `lsm-db-1`):

| Variable | Value | Read by |
|---|---|---|
| `LSMDB_NODE_ID` | pod number + 1 -> `2` | `lsmdb_node` |
| `LSMDB_PORT` | `50051` (`values.yaml: node.port`) | `lsmdb_node` |
| `LSMDB_ADVERTISE_HOST` | `lsm-db-1.lsm-db-headless` | `lsmdb_node` |
| `LSMDB_PEERS` | `1=lsm-db-0.lsm-db-headless:50051,3=lsm-db-2.lsm-db-headless:50051` (this node excluded) | `lsmdb_node` |
| `LSMDB_DATA_DIR` | `/data` (the per-pod PersistentVolume) | storage config layer (not read yet) |

The real node (`lsmdb_node` from `rpc/node_main.cpp`) reads these variables directly. The placeholder node (`placeholder-node/`) is maintained as a standalone testing harness. Raft and consensus read `LSMDB_NODE_ID`, `LSMDB_PEERS`, and `LSMDB_HEADLESS_SERVICE` natively.

## Running the baseline

1. One-time tool setup: Docker, kubectl, kind, Helm (see team onboarding notes).
2. Create a local cluster: `kind create cluster --name lsm-dev`
3. From this directory: `cd deploy && chmod +x smoke-test.sh && ./smoke-test.sh`

This builds the `lsm-db-node:dev` image (using `deploy/Dockerfile` for `lsmdb_node` or `placeholder-node/` if building stand-alone), loads it into the cluster, deploys the Helm chart, and verifies all 3 pods come up, their full DNS names resolve, each node accepts a TCP connection from a separate pod, and the StatefulSet reports 3/3 ready. The script fails (non-zero exit) if any check fails.

## Running the lifecycle test

After `smoke-test.sh` has successfully deployed the cluster:

```bash
chmod +x tests/lifecycle-test.sh
./tests/lifecycle-test.sh
```

This deletes `lsm-db-1` (simulating a crash), confirms Kubernetes recreates it under the *same name* with a *new* internal pod UID, confirms its persistent volume reattached with data intact, and confirms DNS still resolves correctly afterward.

## Production and Testing Characteristics

- The production container image is built by `deploy/Dockerfile`, compiling `lsmdb_node` with gRPC and Protobuf, running as non-root UID `10001` matching `fsGroup: 10001`.
- `storageClassName: standard` is configured for local `kind` development; for cloud deployments, configure SSD-backed storage classes (e.g. `gp3`, `premium-rwo`).
- Node discovery works either via Kubernetes downward API (`LSMDB_HEADLESS_SERVICE`, `LSMDB_POD_NAME`, `LSMDB_NAMESPACE`) or explicit environment injection (`LSMDB_NODE_ID`, `LSMDB_PORT`, `LSMDB_PEERS`).
- `LSMDB_DATA_DIR` mounts `/data` (PersistentVolumeClaim).
- Pods handle SIGTERM cleanly for graceful shutdown.
- Always run `cd deploy` before running `./smoke-test.sh` or `./tests/lifecycle-test.sh`.
