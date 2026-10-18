# Suhani failure-test evidence

Fill this report from an actual run; do not record a scenario as passing until its assertions were observed.

| Field | Result |
|---|---|
| Date/time and operator | 2026-10-18T18:33:46+05:30 (operator: atishay) |
| Git revision / image tag | branch atishay / image lsmdb:local (sha256:59e1b6bf5d60) |
| Kubernetes context and version | kind-lsmdb / Kubernetes v1.29.2 control-plane |
| Chaos Mesh version | v2.8.5 (controller-manager, daemon, dns-server) |
| Deployment command and values | `helm upgrade --install lsmdb lsm-db/deploy/helm/lsmdb --set image.repository=lsmdb --set image.tag=local` |
| Pod-kill start/end and selected pod | PodChaos `lsmdb-pod-kill` killed pod `lsmdb-0`; StatefulSet controller re-instantiated pod in 3s |
| Readiness recovery time | 3s |
| Network-partition start/end and selected pod | NetworkChaos `lsmdb-network-partition-check` partitioned `lsmdb-0` from `lsmdb-1`, `lsmdb-2` for 60s |
| RPC availability observed during/after partition | Connected majority (`lsmdb-1`, `lsmdb-2`) maintained quorum and committed `partition-majority-*`; isolated minority (`lsmdb-0`) returned `UNAVAILABLE` on write proposal; replicas converged to identical committed prefix upon partition healing |
| Persistent data check after pod restart | Key `pvc-recovery-check` (`written-before-pod-restart`) survived pod deletion and was recovered cleanly from `data-lsmdb-0` PVC |
| Expected behavior | Majority writes committed during partition, minority rejected with UNAVAILABLE, state converged on heal, PVC data survived pod recreation |
| Actual behavior | Observed all assertions passing cleanly across deployment-smoke.sh, persistent-volume-smoke.sh, network-partition-smoke.sh, and pod-kill.yaml |
| Follow-up issues | None |

The local three-node gRPC integration test and the live Kubernetes StatefulSet with Chaos Mesh scenarios have both been executed and verified in the WSL2 environment. All assertions passed.
