# RPC / Protobuf Contract (Week 1)

Owner: Vinayak. Consumers: Atishay (storage API), Sarthak and Atishay (Raft RPCs, Weeks 5-6),
Suhani (Helm config, read/write routing). **Any change to `proto/` needs review by one other member.**

## Services

| Service | RPC | Purpose |
|---|---|---|
| `KvService` | `Put`, `Get`, `Delete` | Client -> node key-value API |
| `NodeService` | `Ping`, `GetNodeInfo` | Liveness and node identity (node <-> node and operations) |

Raft RPCs (`RequestVote`, `AppendEntries`) are intentionally **not** defined yet. They will be
added in Week 5/6 as `proto/lsmdb/v1/raft.proto`, reusing `RequestMeta`, `NodeInfo` and `Status`.

## Error model (important)

* **Application outcomes live in the response body** as `Status{code, message, leader_hint}`.
  The gRPC status is `OK` whenever the request reached the handler.
* **A non-OK gRPC status means transport failure** (node down, timeout, message too large).
  The client wrappers convert it into the same `Status` model (`FromGrpcStatus`).

| `StatusCode` | Number | Meaning |
|---|---|---|
| `OK` | 0 | Success |
| `INVALID_ARGUMENT` | 1 | Failed validation (or message above transport limit) |
| `NOT_FOUND` | 2 | Key does not exist (Get only) |
| `NOT_LEADER` | 3 | Reserved for Raft; `leader_hint` tells the client where to go |
| `UNAVAILABLE` | 4 | Node unreachable or deadline exceeded |
| `INTERNAL` | 5 | Storage threw an exception |

Numbers are frozen. Only append new codes.

## Limits (`rpc/include/lsmdb/rpc/validation.h`)

Key 1..1024 bytes. Value 0..1,048,576 bytes. `request_id` at most 128 bytes.
gRPC message cap 2 MiB. Keys and values are arbitrary bytes (embedded NUL is fine).
These limits are my Week 1 defaults, not from the README. The team can change them in one place.

## Semantics

* `Delete` is idempotent: deleting a missing key returns `OK`.
* `Get` of a missing key returns `NOT_FOUND` with an empty value.
* `RequestMeta` is optional. `sender_node_id = 0` means an external client; nodes are numbered from 1.
* Until Raft (Week 6) each node has independent storage.

## Node identity and configuration

`lsmdb_node` takes `--node-id`, `--port`, `--host` (advertised address), `--peers id=host:port,...`.
Each has an environment fallback for Kubernetes: `LSMDB_NODE_ID`, `LSMDB_PORT`,
`LSMDB_ADVERTISE_HOST`, `LSMDB_PEERS`. Flags override environment variables.

**For Suhani (Helm):** derive `LSMDB_NODE_ID` as pod ordinal + 1, so `lsmdb-0` -> 1, `lsmdb-1` -> 2,
`lsmdb-2` -> 3, and set `LSMDB_PEERS` to the other two pods' headless-service DNS names with port 50051.

## Integration points

| With | Point | Status |
|---|---|---|
| Atishay storage API | Implement `StorageBackend` (`storage_backend.h`) as a thin adapter, pass it to `NodeServer` in `node_main.cpp` | `InMemoryBackend` placeholder in use |
| Atishay config layer | Build `NodeOptions` from the config | Flags/env for now |
| Raft (Weeks 5-6) | New `raft.proto`; `NodeClient`-style stubs per peer; `NOT_LEADER` + `leader_hint` | Not started |
| Helm (Suhani) | Env variables above | Contract documented |

## Evolving the contract

Add fields with new numbers, never reuse or renumber. Old nodes ignore unknown fields
(tested in `serialization_test.cpp`).
