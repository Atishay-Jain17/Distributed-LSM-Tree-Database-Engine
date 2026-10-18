#!/usr/bin/env python3
"""
Verification script for Distributed LSM-Tree Web GUI Backend Adapter.
Tests real cluster bootstrapping, node status queries, PUT, GET, DELETE,
leader redirection, and activity logging.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import server


def run_full_verification():
    print("==================================================================")
    print(" 1. Bootstrapping 3-Node Raft Cluster via server.start_cluster_nodes()")
    print("==================================================================")
    ok, msg = server.start_cluster_nodes()
    assert ok, f"Cluster start failed: {msg}"
    print(f" [OK] {msg}")

    print("\n==================================================================")
    print(" 2. Polling Node Status & Consensus State")
    print("==================================================================")
    healthy_nodes = []
    leader_node = None
    term = None

    for node in server.DEFAULT_NODES:
        info = server.get_node_info(node["endpoint"])
        print(f" -> Node {node['node_id']} ({node['endpoint']}): status={info['status']}, role={info['role']}, term={info['term']}, leader_id={info['leader_id']}, peers={len(info['peers'])}")
        assert info["status"] == "healthy", f"Node {node['node_id']} not healthy: {info}"
        assert info["role"] in ["LEADER", "FOLLOWER"]
        if info["role"] == "LEADER":
            leader_node = node["node_id"]
            term = info["term"]
        healthy_nodes.append(info)

    assert leader_node is not None, "No leader detected in cluster!"
    print(f" [OK] Cluster healthy! Active Leader: Node {leader_node}, Term: {term}")

    print("\n==================================================================")
    print(" 3. Testing Real KV Operations against Cluster")
    print("==================================================================")

    # Pick a follower to test redirection to leader
    follower_id = 1 if leader_node != 1 else 2
    follower_endpoint = f"127.0.0.1:5005{follower_id}"
    leader_endpoint = f"127.0.0.1:5005{leader_node}"

    print(f" (a) Testing PUT operation sent to FOLLOWER ({follower_endpoint})...")
    # Simulate API call to handle_api_kv
    class DummyHandler(server.GuiRequestHandler):
        def __init__(self):
            self.response_data = None
            self.status_code = None

        def send_json(self, data, status_code=200):
            self.response_data = data
            self.status_code = status_code

    handler = DummyHandler()

    # PUT
    handler.handle_api_kv({
        "op": "put",
        "key": "sensor:turbofan:rpm",
        "value": "14250",
        "target": follower_endpoint
    })
    res_put = handler.response_data
    print(f"     Response: {json.dumps(res_put, indent=2)}")
    assert res_put["success"] is True, f"PUT failed: {res_put}"
    assert res_put["redirected"] is True, "Follower should have redirected to leader!"
    assert str(leader_node) in res_put["handling_node"], "Handling node should indicate active leader!"
    print(f"     [OK] PUT redirected from follower to leader {leader_node} and committed.")

    # GET from another node (follower or leader)
    other_node_id = 3 if leader_node != 3 else 1
    other_endpoint = f"127.0.0.1:5005{other_node_id}"
    print(f"\n (b) Testing GET operation sent to node {other_endpoint}...")
    handler.handle_api_kv({
        "op": "get",
        "key": "sensor:turbofan:rpm",
        "target": other_endpoint
    })
    res_get = handler.response_data
    print(f"     Response: {json.dumps(res_get, indent=2)}")
    assert res_get["success"] is True, f"GET failed: {res_get}"
    assert res_get["status"] == "FOUND"
    assert res_get["value"] == "14250", f"Expected '14250', got {res_get['value']}"
    print(f"     [OK] GET verified replicated value: '{res_get['value']}'")

    # GET non-existent key
    print(f"\n (c) Testing GET on non-existent key...")
    handler.handle_api_kv({
        "op": "get",
        "key": "non_existent_key_9999",
        "target": leader_endpoint
    })
    res_missing = handler.response_data
    print(f"     Response: status={res_missing['status']}, success={res_missing['success']}")
    assert res_missing["status"] == "NOT_FOUND"
    print("     [OK] Missing key returns NOT_FOUND status correctly.")

    # DELETE key
    print(f"\n (d) Testing DELETE operation...")
    handler.handle_api_kv({
        "op": "delete",
        "key": "sensor:turbofan:rpm",
        "target": follower_endpoint
    })
    res_del = handler.response_data
    print(f"     Response: status={res_del['status']}, success={res_del['success']}")
    assert res_del["success"] is True
    print("     [OK] DELETE committed to cluster.")

    # GET after delete
    print(f"\n (e) Testing GET on deleted key...")
    handler.handle_api_kv({
        "op": "get",
        "key": "sensor:turbofan:rpm",
        "target": leader_endpoint
    })
    res_post_del = handler.response_data
    print(f"     Response: status={res_post_del['status']}")
    assert res_post_del["status"] == "NOT_FOUND"
    print("     [OK] Verified tombstone: Key is NOT_FOUND.")

    print("\n==================================================================")
    print(" 4. Verifying Activity Log Feed")
    print("==================================================================")
    with server.activity_lock:
        events = list(server.activity_log)
    print(f" Logged events count: {len(events)}")
    for ev in events[:5]:
        print(f" -> [{ev['timestamp']}] {ev['op']} key={ev['key']} target={ev['target']} status={ev['status']} ({ev['duration_ms']}ms)")
    assert len(events) >= 5, "Activity feed should record cluster operations"
    print(" [OK] Activity log verified.")

    print("\n==================================================================")
    print(" 5. Testing Cluster Teardown via server.stop_cluster_nodes()")
    print("==================================================================")
    ok, msg = server.stop_cluster_nodes()
    assert ok
    print(f" [OK] {msg}")

    print("\n==================================================================")
    print(" [VERIFICATION COMPLETED] ALL GUI ADAPTER & CLUSTER CHECKS PASSED!")
    print("==================================================================")


if __name__ == "__main__":
    run_full_verification()
