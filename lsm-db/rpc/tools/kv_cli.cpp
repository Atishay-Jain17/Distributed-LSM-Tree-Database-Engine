// kv_cli: tiny client for manual checks and the smoke test.
//
//   kv_cli [--target host:port] [--sender-id N] put <key> <value>
//   kv_cli [--target host:port] get <key>
//   kv_cli [--target host:port] delete <key>
//   kv_cli [--target host:port] ping
//   kv_cli [--target host:port] info
//
// Exit code 0 on STATUS_CODE_OK, 1 otherwise.
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lsmdb/rpc/clients.h"
#include "lsmdb/rpc/node_options.h"
#include "lsmdb/rpc/status.h"

using namespace lsmdb::rpc;

namespace {
int Report(const lsmdb::v1::Status& s) {
  if (IsOk(s)) {
    std::cout << "OK" << std::endl;
    return 0;
  }
  std::cout << "ERROR " << lsmdb::v1::StatusCode_Name(s.code()) << ": " << s.message() << std::endl;
  return 1;
}
void PrintNode(const char* label, const lsmdb::v1::NodeInfo& n) {
  std::cout << label << " node_id=" << n.node_id() << " host=" << n.host() << " port=" << n.port()
            << std::endl;
}
int Usage() {
  std::cerr << "usage: kv_cli [--target host:port] [--sender-id N] "
               "put <k> <v> | get <k> | delete <k> | ping | info\n";
  return 2;
}

bool FollowLeader(const lsmdb::v1::Status& status, std::string* target, std::set<std::string>* visited) {
  if (status.code() != lsmdb::v1::STATUS_CODE_NOT_LEADER) return false;
  if (!status.has_leader_hint() || status.leader_hint().host().empty() || status.leader_hint().port() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return true;  // Election/heartbeat has not published a leader endpoint yet; retry this node.
  }
  const std::string next = status.leader_hint().host() + ":" + std::to_string(status.leader_hint().port());
  if (next == *target) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return true;
  }
  if (!visited->insert(next).second) return false;
  std::cerr << "redirecting to Raft leader " << status.leader_hint().node_id() << " at " << next << std::endl;
  *target = next;
  return true;
}

lsmdb::v1::Status PutRouted(std::string target, ClientOptions opts, const std::string& key, const std::string& value) {
  std::set<std::string> visited{target};
  for (int attempt = 0; attempt < 8; ++attempt) {
    auto status = KvClient(MakeChannel(target), opts).Put(key, value);
    if (!FollowLeader(status, &target, &visited)) return status;
  }
  return MakeStatus(lsmdb::v1::STATUS_CODE_UNAVAILABLE, "leader redirect limit reached");
}

KvClient::GetResult GetRouted(std::string target, ClientOptions opts, const std::string& key) {
  std::set<std::string> visited{target};
  for (int attempt = 0; attempt < 8; ++attempt) {
    auto result = KvClient(MakeChannel(target), opts).Get(key);
    if (!FollowLeader(result.status, &target, &visited)) return result;
  }
  return {MakeStatus(lsmdb::v1::STATUS_CODE_UNAVAILABLE, "leader redirect limit reached"), {}};
}

lsmdb::v1::Status DeleteRouted(std::string target, ClientOptions opts, const std::string& key) {
  std::set<std::string> visited{target};
  for (int attempt = 0; attempt < 8; ++attempt) {
    auto status = KvClient(MakeChannel(target), opts).Delete(key);
    if (!FollowLeader(status, &target, &visited)) return status;
  }
  return MakeStatus(lsmdb::v1::STATUS_CODE_UNAVAILABLE, "leader redirect limit reached");
}
}  // namespace

int main(int argc, char** argv) {
  std::string target = "127.0.0.1:50051";
  ClientOptions opts;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--target" && i + 1 < argc) {
      target = argv[++i];
    } else if (a == "--sender-id" && i + 1 < argc) {
      try {
        opts.sender_node_id = ParseUint32Argument(argv[++i], "sender id");
      } catch (const std::invalid_argument& e) {
        std::cerr << e.what() << std::endl;
        return Usage();
      }
    } else {
      args.push_back(a);
    }
  }
  if (args.empty()) return Usage();

  auto channel = MakeChannel(target);
  const std::string& cmd = args[0];

  if (cmd == "put" && args.size() == 3) return Report(PutRouted(target, opts, args[1], args[2]));
  if (cmd == "delete" && args.size() == 2) return Report(DeleteRouted(target, opts, args[1]));
  if (cmd == "get" && args.size() == 2) {
    auto r = GetRouted(target, opts, args[1]);
    if (!IsOk(r.status)) return Report(r.status);
    std::cout << "VALUE=" << r.value << std::endl;
    return 0;
  }
  if (cmd == "ping" && args.size() == 1) {
    auto r = NodeClient(channel, opts).Ping();
    if (!IsOk(r.status)) return Report(r.status);
    PrintNode("PONG", r.responder);
    return 0;
  }
  if (cmd == "info" && args.size() == 1) {
    auto r = NodeClient(channel, opts).GetNodeInfo();
    if (!IsOk(r.status)) return Report(r.status);
    PrintNode("SELF", r.self);
    for (const auto& p : r.peers) PrintNode("PEER", p);
    std::cout << "RAFT role=" << (r.is_leader ? "LEADER" : "FOLLOWER")
              << " term=" << r.current_term << " leader_id=" << r.leader_id
              << " commit_index=" << r.commit_index << " last_applied=" << r.last_applied << std::endl;
    return 0;
  }
  return Usage();
}
