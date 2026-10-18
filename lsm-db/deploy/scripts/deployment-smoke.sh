#!/usr/bin/env bash
set -euo pipefail

release="${1:-lsmdb}"
namespace="${2:-default}"
kubectl -n "$namespace" rollout status "statefulset/$release" --timeout=180s

# Wait for the persistent peer RPCs and election timers to establish one leader.
leader_count=0
for attempt in $(seq 1 60); do
  leader_count=0
  for ordinal in 0 1 2; do
    info="$(kubectl -n "$namespace" exec "$release-$ordinal" -- /app/kv_cli info)"
    if grep -q 'RAFT role=LEADER' <<<"$info"; then ((leader_count += 1)); fi
  done
  [[ "$leader_count" -eq 1 ]] && break
  sleep 1
done
[[ "$leader_count" -eq 1 ]] || { echo "expected exactly one Raft leader, found $leader_count" >&2; exit 1; }

for ordinal in 0 1 2; do
  pod="$release-$ordinal"
  kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target "127.0.0.1:50051" ping
  key="smoke-$ordinal"
  kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target "127.0.0.1:50051" put "$key" "node-$ordinal"
  value="$(kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target "127.0.0.1:50051" get "$key")"
  [[ "$value" == *"VALUE=node-$ordinal"* ]] || { echo "GET mismatch on $pod: $value" >&2; exit 1; }
  kubectl -n "$namespace" exec "$pod" -- /app/kv_cli --target "127.0.0.1:50051" delete "$key"
done

# Allow the commit heartbeat to reach and apply on every replica before the final check.
replicated=false
for attempt in $(seq 1 30); do
  replicated=true
  common_index=""
  for ordinal in 0 1 2; do
    info="$(kubectl -n "$namespace" exec "$release-$ordinal" -- /app/kv_cli info)"
    if [[ "$info" =~ commit_index=([0-9]+)[[:space:]]last_applied=([0-9]+) ]]; then
      commit_index="${BASH_REMATCH[1]}"
      last_applied="${BASH_REMATCH[2]}"
      if [[ "$commit_index" != "$last_applied" ]]; then replicated=false; fi
      if [[ -n "$common_index" && "$commit_index" != "$common_index" ]]; then replicated=false; fi
      common_index="$commit_index"
    else
      replicated=false
    fi
  done
  [[ "$replicated" == true ]] && break
  sleep 1
done
[[ "$replicated" == true ]] || { echo "replicas did not apply the same committed prefix" >&2; exit 1; }

echo "PASS: three Raft nodes elected one leader, accepted CLI CRUD through follower redirects, and applied their committed prefixes"
