#!/usr/bin/env bash
set -euo pipefail

release="${1:-lsmdb}"
namespace="${2:-default}"
pod="$release-0"
key="pvc-recovery-check"
value="written-before-pod-restart"

kubectl -n "$namespace" rollout status "statefulset/$release" --timeout=180s
kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target 127.0.0.1:50051 put "$key" "$value"
kubectl -n "$namespace" delete pod "$pod" --wait=true
kubectl -n "$namespace" wait "pod/$pod" --for=condition=Ready --timeout=180s
actual="$(kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target 127.0.0.1:50051 get "$key")"
[[ "$actual" == *"VALUE=$value"* ]] || { echo "value was not recovered after restart: $actual" >&2; exit 1; }
kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target 127.0.0.1:50051 delete "$key"
echo "PASS: node recovered its local value after pod restart on the StatefulSet volume"
