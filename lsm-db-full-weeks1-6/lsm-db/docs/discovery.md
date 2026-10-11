# Peer discovery and peer health (Week 5)

**Identity rule:** `node_id = StatefulSet pod ordinal + 1` (`lsmdb-0` -> 1, `lsmdb-1` -> 2, `lsmdb-2` -> 3).
Peer addresses are the stable pod DNS names `lsmdb-N.<service>.<namespace>.svc.cluster.local:<port>`.

**What the node reads (all environment variables):**

| Variable | Meaning | Default |
|---|---|---|
| `LSMDB_HEADLESS_SERVICE` | name of the headless Service; its presence turns discovery on | none |
| `LSMDB_POD_NAME` | pod name (falls back to `HOSTNAME`, which Kubernetes sets) | `HOSTNAME` |
| `LSMDB_NAMESPACE` | namespace (downward API `metadata.namespace`) | `default` |
| `LSMDB_REPLICAS` | StatefulSet replica count | 3 |
| `LSMDB_PORT` | gRPC port | 50051 |
| `LSMDB_CLUSTER_DOMAIN` | cluster DNS suffix | `cluster.local` |

Explicit `--node-id` / `LSMDB_NODE_ID` still wins over discovery (local development).

**For Suhani (Helm), required settings:**
* a **headless Service** (`clusterIP: None`) with `publishNotReadyAddresses: true`, so peers can
  resolve each other's DNS names before they are Ready (otherwise nodes cannot find each other
  during startup);
* the StatefulSet's `serviceName` equal to `LSMDB_HEADLESS_SERVICE`;
* `LSMDB_NAMESPACE` from the downward API (`fieldRef: metadata.namespace`).

**PeerManager.** One monitor thread pings each peer (`NodeService.Ping`) every 500 ms with a 300 ms
timeout. Two consecutive failures mark a peer DOWN; one success marks it UP. A peer that answers with
the wrong node id is treated as down. Channels use short reconnect backoff (200 ms to 1 s) and re-resolve
DNS every second, because restarted pods get new IPs. Start order does not matter.
The Raft code gets a peer's channel from `ChannelFor(id)` and reachability from `StateOf(id)` / the callback.
