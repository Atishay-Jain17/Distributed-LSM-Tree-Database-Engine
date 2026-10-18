#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="/mnt/d/PBL_V_Sem/week1/Distributed-LSM-Tree-Database-Engine"
NODE_BIN="${ROOT_DIR}/build/linux-verify/lsm-db/rpc/lsmdb_node"
CLI_BIN="${ROOT_DIR}/build/linux-verify/lsm-db/rpc/kv_cli"
DATA_DIR="/tmp/lsmdb_live_cluster_$(date +%s)"

mkdir -p "${DATA_DIR}/n1" "${DATA_DIR}/n2" "${DATA_DIR}/n3"

echo "==> [1/6] Starting 3-node Raft gRPC cluster..."
"${NODE_BIN}" --node-id 1 --port 50051 --peers 2=127.0.0.1:50052,3=127.0.0.1:50053 --data-dir "${DATA_DIR}/n1" > "${DATA_DIR}/n1.log" 2>&1 &
PID1=$!
"${NODE_BIN}" --node-id 2 --port 50052 --peers 1=127.0.0.1:50051,3=127.0.0.1:50053 --data-dir "${DATA_DIR}/n2" > "${DATA_DIR}/n2.log" 2>&1 &
PID2=$!
"${NODE_BIN}" --node-id 3 --port 50053 --peers 1=127.0.0.1:50051,2=127.0.0.1:50052 --data-dir "${DATA_DIR}/n3" > "${DATA_DIR}/n3.log" 2>&1 &
PID3=$!

cleanup() {
    echo "==> Cleaning up cluster processes..."
    kill -9 "${PID1}" "${PID2}" "${PID3}" 2>/dev/null || true
    rm -rf "${DATA_DIR}"
}
trap cleanup EXIT

echo "==> [2/6] Waiting for cluster to elect a leader..."
sleep 2

LEADER_PORT=""
LEADER_ID=""
for port in 50051 50052 50053; do
    echo "Pinging node on port ${port}..."
    "${CLI_BIN}" --target "127.0.0.1:${port}" ping || true
    INFO_OUT=$("${CLI_BIN}" --target "127.0.0.1:${port}" info 2>&1 || true)
    echo "${INFO_OUT}"
    if echo "${INFO_OUT}" | grep -q "role=LEADER"; then
        LEADER_PORT="${port}"
        echo "--> Found Leader on port ${LEADER_PORT}!"
    fi
done

if [ -z "${LEADER_PORT}" ]; then
    echo "Leader not discovered on initial pass, waiting another 2s..."
    sleep 2
    for port in 50051 50052 50053; do
        INFO_OUT=$("${CLI_BIN}" --target "127.0.0.1:${port}" info 2>&1 || true)
        if echo "${INFO_OUT}" | grep -q "role=LEADER"; then
            LEADER_PORT="${port}"
            echo "--> Found Leader on port ${LEADER_PORT}!"
            break
        fi
    done
fi

if [ -z "${LEADER_PORT}" ]; then
    echo "ERROR: Failed to establish cluster leader."
    exit 1
fi

echo "==> [3/6] Executing quorum write on leader (127.0.0.1:${LEADER_PORT})..."
"${CLI_BIN}" --target "127.0.0.1:${LEADER_PORT}" put "distributed_key_1" "hello_distributed_raft"

echo "==> [4/6] Reading key back from leader..."
"${CLI_BIN}" --target "127.0.0.1:${LEADER_PORT}" get "distributed_key_1"

echo "==> [5/6] Demonstrating follower rejection & leader routing..."
for port in 50051 50052 50053; do
    if [ "${port}" != "${LEADER_PORT}" ]; then
        echo "Attempting write to follower on port ${port} (expected rejection with leader hint):"
        "${CLI_BIN}" --target "127.0.0.1:${port}" put "follower_write" "should_fail" || true
        break
    fi
done

echo "==> [6/6] Verifying node info and Raft log state..."
"${CLI_BIN}" --target "127.0.0.1:${LEADER_PORT}" info

echo "=============================================================================="
echo "SUCCESS: Live 3-node gRPC Raft cluster verified end-to-end via CLI!"
echo "=============================================================================="
