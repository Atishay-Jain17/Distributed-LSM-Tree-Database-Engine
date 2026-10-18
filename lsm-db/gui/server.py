#!/usr/bin/env python3
"""
Distributed LSM-Tree Database Engine - Web GUI Backend Adapter
--------------------------------------------------------------
A lightweight HTTP/REST adapter that forwards requests directly to the
real `kv_cli` and `lsmdb_node` processes. Zero extra pip dependencies.

Binds strictly to 127.0.0.1 (localhost).
User inputs are strictly validated and passed as discrete argv vectors (no shell execution).
"""

import atexit
import http.server
import json
import os
import re
import socketserver
import subprocess
import sys
import threading
import time
from urllib.parse import urlparse, parse_qs

PORT = int(os.environ.get("PORT", 8080))
BIND_HOST = "127.0.0.1"  # Bound strictly to localhost
BASE_DIR = os.path.abspath(os.path.dirname(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")
ROOT_DIR = os.path.abspath(os.path.join(BASE_DIR, "..", ".."))

# Resolve binary paths
DEFAULT_LINUX_CLI = os.path.join(ROOT_DIR, "build", "linux-verify", "lsm-db", "rpc", "kv_cli")
DEFAULT_LINUX_NODE = os.path.join(ROOT_DIR, "build", "linux-verify", "lsm-db", "rpc", "lsmdb_node")

WSL_ROOT = "/mnt/d/PBL_V_Sem/week1/Distributed-LSM-Tree-Database-Engine"
WSL_CLI = f"{WSL_ROOT}/build/linux-verify/lsm-db/rpc/kv_cli"
WSL_NODE = f"{WSL_ROOT}/build/linux-verify/lsm-db/rpc/lsmdb_node"

DEFAULT_NODES = [
    {"node_id": 1, "port": 50051, "endpoint": "127.0.0.1:50051"},
    {"node_id": 2, "port": 50052, "endpoint": "127.0.0.1:50052"},
    {"node_id": 3, "port": 50053, "endpoint": "127.0.0.1:50053"},
]

DEFAULT_CLUSTER_CONFIG = [
    {"node_id": 1, "port": 50051, "peers": "2=127.0.0.1:50052,3=127.0.0.1:50053", "data_dir": "/tmp/lsmdb_gui_nodes/n1"},
    {"node_id": 2, "port": 50052, "peers": "1=127.0.0.1:50051,3=127.0.0.1:50053", "data_dir": "/tmp/lsmdb_gui_nodes/n2"},
    {"node_id": 3, "port": 50053, "peers": "1=127.0.0.1:50051,2=127.0.0.1:50052", "data_dir": "/tmp/lsmdb_gui_nodes/n3"},
]

activity_log = []
activity_lock = threading.Lock()
cluster_procs = {}
cluster_lock = threading.Lock()


def record_activity(op_type, key, value, target, handling_node, status, message, duration_ms):
    with activity_lock:
        entry = {
            "id": len(activity_log) + 1,
            "timestamp": time.strftime("%H:%M:%S"),
            "op": op_type,
            "key": key or "-",
            "value": value if value is not None else "-",
            "target": target,
            "handling_node": handling_node,
            "status": status,  # "OK", "ERROR", "NOT_FOUND", "REDIRECTED"
            "message": message,
            "duration_ms": round(duration_ms, 2),
        }
        activity_log.insert(0, entry)
        if len(activity_log) > 150:
            activity_log.pop()


def run_cli_command(args, timeout_sec=6.0):
    """
    Executes kv_cli using discrete argv vector (shell=False).
    Guarantees no shell-string execution.
    """
    is_windows = sys.platform.startswith("win")
    start_time = time.time()

    if is_windows:
        cmd = ["wsl.exe", "-d", "Ubuntu-24.04", "-u", "root", "-e", WSL_CLI] + args
    else:
        cmd = [DEFAULT_LINUX_CLI] + args

    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_sec,
            shell=False,
        )
        duration_ms = (time.time() - start_time) * 1000.0
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip(), duration_ms
    except subprocess.TimeoutExpired:
        duration_ms = (time.time() - start_time) * 1000.0
        return 124, "", "Operation timed out", duration_ms
    except Exception as e:
        duration_ms = (time.time() - start_time) * 1000.0
        return 1, "", str(e), duration_ms


def get_node_info(endpoint):
    code, stdout, stderr, _ = run_cli_command(["--target", endpoint, "info"])
    raw_combined = (stdout + "\n" + stderr).strip()

    if code != 0 or not stdout:
        return {
            "endpoint": endpoint,
            "status": "unreachable",
            "role": "UNKNOWN",
            "error": raw_combined or "Connection refused / node down",
            "term": None,
            "commit_index": None,
            "last_applied": None,
            "leader_id": None,
            "peers": [],
        }

    info = {
        "endpoint": endpoint,
        "status": "healthy",
        "role": "FOLLOWER",
        "error": None,
        "term": 0,
        "commit_index": 0,
        "last_applied": 0,
        "leader_id": 0,
        "peers": [],
    }

    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("RAFT"):
            m_role = re.search(r"role=([A-Z]+)", line)
            if m_role:
                info["role"] = m_role.group(1)
            m_term = re.search(r"term=(\d+)", line)
            if m_term:
                info["term"] = int(m_term.group(1))
            m_leader = re.search(r"leader_id=(\d+)", line)
            if m_leader:
                info["leader_id"] = int(m_leader.group(1))
            m_commit = re.search(r"commit_index=(\d+)", line)
            if m_commit:
                info["commit_index"] = int(m_commit.group(1))
            m_applied = re.search(r"last_applied=(\d+)", line)
            if m_applied:
                info["last_applied"] = int(m_applied.group(1))
        elif line.startswith("SELF"):
            m_id = re.search(r"node_id=(\d+)", line)
            if m_id:
                info["node_id"] = int(m_id.group(1))
        elif line.startswith("PEER"):
            m_p = re.search(r"node_id=(\d+)\s+host=([^\s]+)\s+port=(\d+)", line)
            if m_p:
                info["peers"].append({
                    "node_id": int(m_p.group(1)),
                    "host": m_p.group(2),
                    "port": int(m_p.group(3)),
                })

    return info


def start_single_node(node_id):
    global cluster_procs
    is_windows = sys.platform.startswith("win")
    cfg = next((c for c in DEFAULT_CLUSTER_CONFIG if c["node_id"] == node_id), None)
    if not cfg:
        return False, f"Unknown node id {node_id}"

    with cluster_lock:
        if node_id in cluster_procs:
            proc = cluster_procs[node_id]
            if proc.poll() is None:
                return True, f"Node {node_id} already running"

        if is_windows:
            # Prepare data directory safely in WSL first
            subprocess.run(["wsl.exe", "-d", "Ubuntu-24.04", "-u", "root", "-e", "mkdir", "-p", cfg["data_dir"]], shell=False)
            cmd = [
                "wsl.exe", "-d", "Ubuntu-24.04", "-u", "root", "-e",
                WSL_NODE,
                "--node-id", str(cfg["node_id"]),
                "--port", str(cfg["port"]),
                "--peers", cfg["peers"],
                "--data-dir", cfg["data_dir"],
            ]
        else:
            os.makedirs(cfg["data_dir"], exist_ok=True)
            cmd = [
                DEFAULT_LINUX_NODE,
                "--node-id", str(cfg["node_id"]),
                "--port", str(cfg["port"]),
                "--peers", cfg["peers"],
                "--data-dir", cfg["data_dir"],
            ]

        try:
            p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, shell=False)
            cluster_procs[node_id] = p
        except Exception as e:
            return False, str(e)

    record_activity("NODE", f"node_{node_id}", None, f"127.0.0.1:{cfg['port']}", f"Node {node_id}", "STARTED", f"Started node {node_id}", 100)
    return True, f"Node {node_id} started"


def stop_single_node(node_id):
    global cluster_procs
    is_windows = sys.platform.startswith("win")
    cfg = next((c for c in DEFAULT_CLUSTER_CONFIG if c["node_id"] == node_id), None)
    if not cfg:
        return False, f"Unknown node id {node_id}"

    with cluster_lock:
        proc = cluster_procs.pop(node_id, None)
        if proc:
            try:
                proc.terminate()
                proc.wait(timeout=1.0)
            except Exception:
                try: proc.kill()
                except Exception: pass

        # Also kill by port in WSL/Linux for thoroughness
        pattern = f"port {cfg['port']}"
        if is_windows:
            subprocess.run(["wsl.exe", "-d", "Ubuntu-24.04", "-u", "root", "-e", "pkill", "-f", pattern], shell=False, stderr=subprocess.DEVNULL)
        else:
            subprocess.run(["pkill", "-f", pattern], shell=False, stderr=subprocess.DEVNULL)

    record_activity("NODE", f"node_{node_id}", None, f"127.0.0.1:{cfg['port']}", f"Node {node_id}", "STOPPED", f"Stopped node {node_id}", 100)
    return True, f"Node {node_id} stopped"


def start_cluster_nodes():
    stop_cluster_nodes()
    for cfg in DEFAULT_CLUSTER_CONFIG:
        ok, msg = start_single_node(cfg["node_id"])
        if not ok:
            return False, msg

    time.sleep(2.0)
    record_activity("CLUSTER", "-", None, "all", "local", "STARTED", "Spawned 3-node Raft cluster on 50051..50053", 2000)
    return True, "Cluster started successfully"


def stop_cluster_nodes():
    global cluster_procs
    is_windows = sys.platform.startswith("win")
    with cluster_lock:
        for nid, proc in list(cluster_procs.items()):
            try:
                proc.terminate()
                proc.wait(timeout=1.0)
            except Exception:
                try: proc.kill()
                except Exception: pass
        cluster_procs.clear()

        # Extra safety cleanup of local test nodes
        try:
            if is_windows:
                subprocess.run(["wsl.exe", "-d", "Ubuntu-24.04", "-u", "root", "-e", "pkill", "-f", "lsmdb_node"], shell=False, stderr=subprocess.DEVNULL)
            else:
                subprocess.run(["pkill", "-f", "lsmdb_node"], shell=False, stderr=subprocess.DEVNULL)
        except Exception:
            pass

    record_activity("CLUSTER", "-", None, "all", "local", "STOPPED", "Terminated local node processes", 100)
    return True, "Cluster stopped"


atexit.register(stop_cluster_nodes)


class GuiRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=STATIC_DIR, **kwargs)

    def log_message(self, format, *args):
        try:
            msg = format % args
            if "/api/" in msg:
                sys.stdout.write(f"[API] {msg}\n")
        except Exception:
            pass

    def send_json(self, data, status_code=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
            return
        elif parsed.path == "/api/status":
            self.handle_api_status(parsed)
        elif parsed.path == "/api/activity":
            with activity_lock:
                self.send_json({"events": activity_log})
        elif parsed.path in ["/", "/index.html"]:
            self.path = "/index.html"
            return super().do_GET()
        else:
            return super().do_GET()

    def do_POST(self):
        parsed = urlparse(self.path)
        content_len = int(self.headers.get("Content-Length", 0))
        body_bytes = self.rfile.read(content_len) if content_len > 0 else b"{}"
        try:
            payload = json.loads(body_bytes.decode("utf-8")) if body_bytes else {}
        except Exception:
            payload = {}

        if parsed.path == "/api/kv":
            self.handle_api_kv(payload)
        elif parsed.path == "/api/cluster/start":
            ok, msg = start_cluster_nodes()
            self.send_json({"success": ok, "message": msg})
        elif parsed.path == "/api/cluster/stop":
            ok, msg = stop_cluster_nodes()
            self.send_json({"success": ok, "message": msg})
        elif parsed.path == "/api/node/start":
            node_id = int(payload.get("node_id", 0))
            ok, msg = start_single_node(node_id)
            self.send_json({"success": ok, "message": msg})
        elif parsed.path == "/api/node/stop":
            node_id = int(payload.get("node_id", 0))
            ok, msg = stop_single_node(node_id)
            self.send_json({"success": ok, "message": msg})
        else:
            self.send_json({"error": "Endpoint not found"}, 404)

    def handle_api_status(self, parsed):
        params = parse_qs(parsed.query)
        custom_target = params.get("target", [None])[0]

        nodes_to_poll = list(DEFAULT_NODES)
        if custom_target and not any(n["endpoint"] == custom_target for n in nodes_to_poll):
            # Validate custom target format
            if re.match(r"^[0-9a-zA-Z._-]+:[0-9]{1,5}$", custom_target):
                nodes_to_poll.append({"node_id": 0, "port": 0, "endpoint": custom_target})

        results = []
        leader_id = None
        current_term = None
        has_healthy_node = False

        for node in nodes_to_poll:
            info = get_node_info(node["endpoint"])
            info["configured_id"] = node["node_id"]
            if info["status"] == "healthy":
                has_healthy_node = True
                if info["role"] == "LEADER":
                    leader_id = info.get("node_id", node["node_id"])
                    current_term = info.get("term")
                elif leader_id is None and info.get("leader_id"):
                    leader_id = info.get("leader_id")
                    current_term = info.get("term")
            results.append(info)

        self.send_json({
            "connected": has_healthy_node,
            "leader_id": leader_id,
            "current_term": current_term,
            "nodes": results,
            "timestamp": time.strftime("%H:%M:%S"),
        })

    def handle_api_kv(self, payload):
        op = (payload.get("op") or "").lower().strip()
        key = str(payload.get("key", "")).strip()
        val = str(payload.get("value", ""))
        target = str(payload.get("target", "127.0.0.1:50051")).strip()

        # Input sanitization and safety enforcement
        if op not in ["put", "get", "delete"]:
            self.send_json({"success": False, "error": "Operation must be put, get, or delete"}, 400)
            return

        if not key or "\x00" in key or len(key) > 512:
            self.send_json({"success": False, "error": "Invalid key parameter"}, 400)
            return

        if "\x00" in val or len(val) > 65536:
            self.send_json({"success": False, "error": "Invalid value parameter"}, 400)
            return

        if not re.match(r"^[0-9a-zA-Z._-]+:[0-9]{1,5}$", target):
            self.send_json({"success": False, "error": "Invalid target endpoint format"}, 400)
            return

        args = ["--target", target, op, key]
        if op == "put":
            args.append(val)

        code, stdout, stderr, dur_ms = run_cli_command(args)
        raw_combined = (stdout + "\n" + stderr).strip()

        # Parse redirection hint
        handling_node = target
        redirected = False
        m_redir = re.search(r"redirecting to Raft leader (\d+) at ([^\s]+)", raw_combined)
        if m_redir:
            redirected = True
            handling_node = f"{m_redir.group(2)} (Leader {m_redir.group(1)})"

        success = (code == 0)
        status_label = "OK" if success else "ERROR"
        result_value = None

        if op == "get":
            if "VALUE=" in raw_combined:
                result_value = raw_combined.split("VALUE=", 1)[1].splitlines()[0].strip()
                status_label = "FOUND"
                success = True
            elif "NOT_FOUND" in raw_combined:
                status_label = "NOT_FOUND"
                result_value = None
                success = True

        if not success:
            if "UNAVAILABLE" in raw_combined:
                status_label = "UNAVAILABLE"
            elif "INVALID_ARGUMENT" in raw_combined:
                status_label = "INVALID"
            elif "NOT_LEADER" in raw_combined:
                status_label = "NOT_LEADER"

        record_activity(
            op_type=op.upper(),
            key=key,
            value=val if op == "put" else (result_value if op == "get" else None),
            target=target,
            handling_node=handling_node,
            status=status_label,
            message=raw_combined,
            duration_ms=dur_ms,
        )

        self.send_json({
            "success": success,
            "op": op.upper(),
            "key": key,
            "value": result_value,
            "status": status_label,
            "target": target,
            "handling_node": handling_node,
            "redirected": redirected,
            "raw_output": raw_combined,
            "duration_ms": round(dur_ms, 2),
        })


def main():
    os.chdir(STATIC_DIR)
    print(f"==================================================================")
    print(f" Distributed LSM-Tree Database Engine - Web GUI Adapter Server")
    print(f"==================================================================")
    print(f" Bound strictly to localhost: http://{BIND_HOST}:{PORT}")
    print(f" Serving static assets from: {STATIC_DIR}")
    print(f" Connected to kv_cli runtime (shell=False argv execution)")

    # Check if cluster is reachable; if not, auto-bootstrap local 3-node cluster
    print(" Checking cluster status...")
    st = get_node_info("127.0.0.1:50051")
    if st["status"] != "healthy":
        print(" Local nodes offline. Bootstrapping 3-node Raft cluster...")
        ok, msg = start_cluster_nodes()
        if ok:
            print(" [OK] 3-node Raft cluster spawned on ports 50051, 50052, 50053.")
        else:
            print(f" [WARN] Failed to start cluster nodes: {msg}")
    else:
        print(f" [OK] Existing cluster detected online (Node 1 is {st['role']}).")

    print(f" Dashboard ready! Open http://localhost:{PORT} in your browser.")
    print(f" Press Ctrl+C to terminate.")
    print(f"==================================================================")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((BIND_HOST, PORT), GuiRequestHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down GUI server...")
            httpd.server_close()


if __name__ == "__main__":
    main()
