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

The placeholder node ignores these and still prints its older `POD_NAME`/`NODE_ID`/`PEERS` variables, which are kept
only for the placeholder. Raft (Weeks 5-6) reads `LSMDB_NODE_ID` and `LSMDB_PEERS` unchanged.

## Running the baseline

1. One-time tool setup: Docker, kubectl, kind, Helm (see team onboarding notes).
2. Create a local cluster: `kind create cluster --name lsm-dev`
3. From this directory: `chmod +x smoke-test.sh && ./smoke-test.sh`

This builds the placeholder image, loads it into the cluster, deploys the Helm chart, and verifies all 3 pods come up, their full DNS names resolve, each node accepts a TCP connection from a separate pod, and the StatefulSet reports 3/3 ready. The script fails (non-zero exit) if any check fails. A first run takes a few minutes while the image builds; an upgrade of an existing release rolls the pods one at a time.

## Running the lifecycle test

After `smoke-test.sh` has successfully deployed the cluster:

```bash
chmod +x tests/lifecycle-test.sh
./tests/lifecycle-test.sh
```

This deletes `lsm-db-1` (simulating a crash), confirms Kubernetes recreates it under the *same name* with a *new* internal pod UID, confirms its persistent volume reattached with data intact, and confirms DNS still resolves correctly afterward.

## Known limitations / what's NOT yet covered

- The placeholder binary is a bare TCP echo server, not the real gRPC service — it exists purely to validate infrastructure, not application logic.
- `storageClassName: standard` only works against `kind`'s local-path-provisioner; this is not portable storage and must be reconfigured for any real multi-machine deployment.
- `LSMDB_PEERS` is set but not yet used for anything beyond the node's peer info, since Raft does not exist yet.
- `LSMDB_DATA_DIR` points at the mounted volume, but the storage engine does not read it until the config layer is integrated.
- The lifecycle test deletes a pod and checks the volume with a marker file; it does not test rescheduling to another node (kind has one node by default).

## Handoff notes for next steps

- To run the real node, replace the placeholder image. The build needs a different Dockerfile: it must build `lsm-db/` (gRPC, protobuf), run as non-root uid `10001` (the chart already sets `fsGroup: 10001` so `/data` is writable), and end with `exec /app/node` or change the container command to the real binary. Set `image.repository`/`image.tag` in `values.yaml`.
- The `LSMDB_*` variables above are the whole configuration interface; the Helm chart should not need structural changes.
- Pods ignore SIGTERM in the placeholder, so `terminationGracePeriodSeconds` is 10; the real node handles SIGTERM and exits cleanly.
- Running `smoke-test.sh` from a directory other than `deploy/` will fail (relative paths) — always `cd deploy` first.
