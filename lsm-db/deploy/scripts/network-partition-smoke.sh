#!/usr/bin/env bash
set -euo pipefail

release="${1:-lsmdb}"
namespace="${2:-default}"
minority_pod="${release}-0"
majority_pod="${release}-1"
chaos_name="${release}-network-partition-check"
run_id="$(date +%s)-$$"
majority_key="partition-majority-${run_id}"
minority_key="partition-minority-${run_id}"

kubectl get crd networkchaos.chaos-mesh.org >/dev/null
kubectl -n "$namespace" rollout status "statefulset/$release" --timeout=180s

cleanup() {
  kubectl -n "$namespace" delete networkchaos "$chaos_name" --ignore-not-found --wait=true >/dev/null || true
}
trap cleanup EXIT INT TERM

kubectl -n "$namespace" exec "$majority_pod" -- /app/kv_cli put "$majority_key" before-partition

kubectl apply -f - <<YAML
apiVersion: chaos-mesh.org/v1alpha1
kind: NetworkChaos
metadata:
  name: ${chaos_name}
  namespace: ${namespace}
spec:
  action: partition
  mode: all
  selector:
    namespaces: [${namespace}]
    labelSelectors:
      statefulset.kubernetes.io/pod-name: ${minority_pod}
  direction: both
  target:
    mode: all
    selector:
      namespaces: [${namespace}]
      labelSelectors:
        app.kubernetes.io/instance: ${release}
      expressionSelectors:
        - key: statefulset.kubernetes.io/pod-name
          operator: NotIn
          values: [${minority_pod}]
  duration: "60s"
YAML

# Give Raft time to detect the isolated peer and elect/retain a majority leader.
sleep 3
majority_info="$(kubectl -n "$namespace" exec "$majority_pod" -- /app/kv_cli info)"
if ! grep -q 'RAFT role=LEADER' <<<"$majority_info"; then
  other_info="$(kubectl -n "$namespace" exec "${release}-2" -- /app/kv_cli info)"
  grep -q 'RAFT role=LEADER' <<<"$other_info" || {
    echo "no leader found in the connected majority during partition" >&2
    exit 1
  }
fi

kubectl -n "$namespace" exec "$majority_pod" -- /app/kv_cli put "$majority_key" during-partition

if minority_output="$(kubectl -n "$namespace" exec "$minority_pod" -- /app/kv_cli put "$minority_key" must-not-be-acknowledged 2>&1)"; then
  echo "isolated minority acknowledged a write: $minority_output" >&2
  exit 1
fi
grep -q 'UNAVAILABLE' <<<"$minority_output" || {
  echo "minority write failed for an unexpected reason: $minority_output" >&2
  exit 1
}

cleanup
trap - EXIT INT TERM

for attempt in $(seq 1 60); do
  converged=true
  for ordinal in 0 1 2; do
    output="$(kubectl -n "$namespace" exec "${release}-${ordinal}" -- /app/kv_cli get "$majority_key" 2>&1)" || {
      converged=false
      break
    }
    if [[ "$output" != *"VALUE=during-partition"* ]]; then converged=false; fi
  done
  [[ "$converged" == true ]] && break
  sleep 1
done
[[ "$converged" == true ]] || { echo "replicas did not converge after partition healing" >&2; exit 1; }

echo "PASS: majority write committed during isolation, minority write was not acknowledged, and replicas converged after healing"
