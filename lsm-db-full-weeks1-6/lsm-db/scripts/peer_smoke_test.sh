#!/usr/bin/env bash
# Week 5 smoke test with REAL processes: startup order, failure detection, reconnect,
# and Kubernetes-style identity derivation.   Run:  bash scripts/peer_smoke_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j"$(nproc)" > /dev/null
NODE=build/rpc/lsmdb_node
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT
FAILS=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
wait_for_log() {  # wait_for_log <file> <text> <seconds>
  for _ in $(seq 1 $(($3 * 10))); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
start_node() {  # start_node <id>
  local id=$1 peers="" other
  for other in 1 2 3; do [[ $other -eq $id ]] && continue; peers+="${peers:+,}${other}=127.0.0.1:$((50050 + other))"; done
  "$NODE" --node-id "$id" --port $((50050 + id)) --peers "$peers" >> "build/peer_node$id.log" 2>&1 &
  LAST_PID=$!
}
rm -f build/peer_node*.log

echo "1. Startup order: start node 1 first, nodes 2 and 3 later"
start_node 1; PIDS+=("$LAST_PID")
wait_for_log build/peer_node1.log "listening" 5 && pass "node 1 is listening" || fail "node 1 did not start"
sleep 1.5
start_node 2; PIDS+=("$LAST_PID")
start_node 3; PIDS+=("$LAST_PID"); PID3=$LAST_PID
wait_for_log build/peer_node1.log "peer 2 .* -> UP" 10 && pass "node 1 sees node 2 UP" || fail "node 1 never saw node 2 UP"
wait_for_log build/peer_node1.log "peer 3 .* -> UP" 10 && pass "node 1 sees node 3 UP" || fail "node 1 never saw node 3 UP"

echo "2. Failure detection: kill node 3"
kill "$PID3"; wait "$PID3" 2>/dev/null || true
wait_for_log build/peer_node1.log "peer 3 UP -> DOWN" 10 && pass "node 1 detects node 3 DOWN" || fail "DOWN not detected"

echo "3. Reconnect: restart node 3 on the same port"
start_node 3; PIDS+=("$LAST_PID")
wait_for_log build/peer_node1.log "peer 3 DOWN -> UP" 15 && pass "node 1 sees node 3 UP again" || fail "reconnect not detected"

echo "4. Kubernetes mode: identity from pod name (peers are unreachable DNS names here, which is fine)"
K8S_OUT=$(HOSTNAME=lsmdb-1 LSMDB_HEADLESS_SERVICE=lsmdb LSMDB_NAMESPACE=demo LSMDB_PORT=50061 timeout 2 "$NODE" 2>&1 || true)
[[ "$K8S_OUT" == *"pod lsmdb-1 -> node 2, 2 peers"* ]] && pass "pod lsmdb-1 became node 2 with 2 peers" || fail "discovery output: $K8S_OUT"
BAD_OUT=$(HOSTNAME=lsmdb-7 LSMDB_HEADLESS_SERVICE=lsmdb timeout 2 "$NODE" 2>&1 || true)
[[ "$BAD_OUT" == *"ordinal 7 >= replicas 3"* ]] && pass "ordinal outside replica count is refused" || fail "bad ordinal output: $BAD_OUT"

if [[ $FAILS -eq 0 ]]; then echo "PEER SMOKE TEST PASSED"; else echo "PEER SMOKE TEST FAILED ($FAILS)"; exit 1; fi
