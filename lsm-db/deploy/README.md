# Kubernetes and Chaos Mesh prototype

The Helm chart deploys three `lsmdb_node` processes with StatefulSet identities, a headless peer DNS service, a client Service, and one PVC per pod. Each process gets a stable numeric ID derived from its ordinal, peer addresses, and a data directory on its volume. The node runtime includes the Raft peer service and quorum-backed storage path.

## Build and deploy

From the repository root:

```bash
docker build -f lsm-db/deploy/Dockerfile -t lsmdb:local .
helm upgrade --install lsmdb lsm-db/deploy/helm/lsmdb --set image.repository=lsmdb --set image.tag=local
bash lsm-db/deploy/scripts/deployment-smoke.sh lsmdb default
bash lsm-db/deploy/scripts/persistent-volume-smoke.sh lsmdb default
bash lsm-db/deploy/scripts/network-partition-smoke.sh lsmdb default
```

The image builds from the repository root, so it includes Atishay's SkipList adapter in Suhani's storage path. For a local cluster, load `lsmdb:local` into the cluster's image cache or push it to a registry and set the chart's `image.repository`/`image.tag`. PVC provisioning requires a default StorageClass or `--set storage.storageClassName=...`.

The Docker builder stage compiles the Linux/gRPC targets and the imported GoogleTest targets, then runs CTest before copying only the node and CLI binaries into the runtime image. A failed Linux compile or test therefore stops the image build.

To reach the gRPC client Service from a workstation:

```bash
kubectl port-forward service/lsmdb 50051:50051
```

Then use `kv_cli --target 127.0.0.1:50051 info` to inspect role/term/commit state, or run the CRUD
commands documented in the project README. The CLI follows a `NOT_LEADER` endpoint hint automatically.

## Failure scenarios

The persistent-volume script writes a value, deletes pod ordinal zero, waits for it to return, and checks that the value survived recovery. The deployment smoke checks one leader and a common applied prefix. Install Chaos Mesh and its CRDs before running failure scenarios. The partition runner isolates pod zero, checks a majority-side write, expects an `UNAVAILABLE` response from the isolated node, heals the partition and verifies replica convergence:

```bash
bash lsm-db/deploy/scripts/network-partition-smoke.sh lsmdb default
```

To apply lower-level scenarios manually, observe pod/service status and RPC results, then remove them:

```bash
kubectl apply -f lsm-db/deploy/chaos/pod-kill.yaml
kubectl get pods -l app.kubernetes.io/name=lsmdb -w
kubectl delete -f lsm-db/deploy/chaos/pod-kill.yaml

kubectl apply -f lsm-db/deploy/chaos/network-partition.yaml
kubectl get pods -l app.kubernetes.io/name=lsmdb -w
kubectl delete -f lsm-db/deploy/chaos/network-partition.yaml
```

Record real observations in `lsm-db/evidence/suhani/failure-test-report.md`. The networked Raft code and cluster integration test are present, but neither the Docker image nor a live Kubernetes/Chaos Mesh run has been verified in this workstation environment. Keep the report blank until actual runs provide evidence.
