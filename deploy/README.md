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
| Tue | Configuration injection | `env:` block in the StatefulSet — `POD_NAME`, `NODE_ID`, `NODE_PORT`, and now `PEERS` (a generated list of every node's stable DNS address) |
| Wed | Lifecycle tests | `tests/lifecycle-test.sh` |
| Thu | Deployment handoff | This README |

## Architecture decisions and why

- **StatefulSet, not Deployment**: gives each pod a permanent name (`lsm-db-0/1/2`) and its own PersistentVolumeClaim that survive restarts and rescheduling — required for Raft, where node identity must stay stable.
- **Headless Service**: skips load-balancing so each pod gets its own individually-addressable DNS name, which peer-to-peer consensus protocols need (a normal Service would hide which specific node you're talking to).
- **PEERS environment variable**: generated at Helm template-render time from `replicaCount`, giving each node a ready-made list of every peer's address (`lsm-db-0.lsm-db-headless:50051,lsm-db-1.lsm-db-headless:50051,...`) without needing an external service registry. This is what Raft's peer discovery will read from once it's wired in.
- **Storage class**: defaults to `standard` (kind's local-path-provisioner) for local development. This must be changed to a real cloud provider's storage class before this chart is used outside of local `kind` testing — noted explicitly in `values.yaml`.

## Running the baseline

1. One-time tool setup: Docker, kubectl, kind, Helm (see team onboarding notes).
2. Create a local cluster: `kind create cluster --name lsm-dev`
3. From this directory: `chmod +x smoke-test.sh && ./smoke-test.sh`

This builds the placeholder image, loads it into the cluster, deploys the Helm chart, and verifies all 3 pods come up, their DNS resolves individually, and each responds on its port.

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
- `PEERS` is generated but not yet consumed by any Raft logic, since that doesn't exist yet — it's ready for whoever implements peer discovery to read from.

## Handoff notes for next steps

- Whoever wires in the real gRPC node binary can replace `placeholder-node/` entirely — the Dockerfile's build stage and the Helm chart around it should not need structural changes, only the binary and its listening behavior.
- The `PEERS` env var is the natural place to start Raft peer discovery from.
- Running `smoke-test.sh` from a directory other than `deploy/` will fail (relative paths) — always `cd deploy` first.
