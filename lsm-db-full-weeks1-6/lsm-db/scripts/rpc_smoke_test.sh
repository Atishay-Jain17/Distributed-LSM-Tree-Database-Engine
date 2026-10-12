#!/usr/bin/env bash
# Week 1 smoke test: builds the project, starts THREE node processes with
# distinct identities, and checks client->node requests and node identity.
# Run from anywhere:  bash scripts/rpc_smoke_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j"$(nproc)" > /dev/null

NODE=build/rpc/lsmdb_node
CLI=build/rpc/kv_cli
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT

FAILS=0
expect() {  # expect <description> <needle> <actual>
  if [[ "$3" == *"$2"* ]]; then echo "  PASS  $1"; else echo "  FAIL  $1 (wanted '$2', got '$3')"; FAILS=$((FAILS + 1)); fi
}

echo "Starting 3 nodes on ports 50051, 50052, 50053 ..."
for id in 1 2 3; do
  port=$((50050 + id)); peers=""
  for other in 1 2 3; do
    [[ $other -eq $id ]] && continue
    peers+="${peers:+,}${other}=127.0.0.1:$((50050 + other))"
  done
  "$NODE" --node-id "$id" --port "$port" --peers "$peers" > "build/node$id.log" 2>&1 &
  PIDS+=($!)
done
for id in 1 2 3; do
  for _ in $(seq 1 50); do "$CLI" --target "127.0.0.1:$((50050 + id))" ping > /dev/null 2>&1 && break; sleep 0.1; done
done

for id in 1 2 3; do
  t="127.0.0.1:$((50050 + id))"
  echo "Node $id ($t)"
  expect "ping reports node id $id"       "PONG node_id=$id"       "$($CLI --target "$t" ping)"
  expect "put succeeds"                   "OK"                     "$($CLI --target "$t" put "key-$id" "value-$id")"
  expect "get returns the value"          "VALUE=value-$id"        "$($CLI --target "$t" get "key-$id")"
  expect "delete succeeds"                "OK"                     "$($CLI --target "$t" delete "key-$id")"
  expect "get after delete is NOT_FOUND"  "STATUS_CODE_NOT_FOUND"  "$($CLI --target "$t" get "key-$id" || true)"
  expect "empty key is INVALID_ARGUMENT"  "STATUS_CODE_INVALID_ARGUMENT" "$($CLI --target "$t" put "" "v" || true)"
done

echo "Node identity / peer metadata"
expect "node 1 lists peer 2 and 3" "PEER node_id=3" "$($CLI --target 127.0.0.1:50051 info)"

echo "Isolation (expected until Raft replication in Week 6)"
$CLI --target 127.0.0.1:50051 put shared-key v > /dev/null
expect "key written to node 1 is absent on node 2" "STATUS_CODE_NOT_FOUND" "$($CLI --target 127.0.0.1:50052 get shared-key || true)"

echo "Dead node handling"
kill "${PIDS[2]}"; wait "${PIDS[2]}" 2>/dev/null || true
expect "stopped node reports UNAVAILABLE" "STATUS_CODE_UNAVAILABLE" "$($CLI --target 127.0.0.1:50053 ping || true)"

if [[ $FAILS -eq 0 ]]; then echo "SMOKE TEST PASSED"; else echo "SMOKE TEST FAILED ($FAILS failures)"; exit 1; fi
