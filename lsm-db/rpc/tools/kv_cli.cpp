// kv_cli: tiny client for manual checks and the smoke test.
//
//   kv_cli [--target host:port] [--sender-id N] put <key> <value>
//   kv_cli [--target host:port] get <key>
//   kv_cli [--target host:port] delete <key>
//   kv_cli [--target host:port] ping
//   kv_cli [--target host:port] info
//
// Exit code 0 on STATUS_CODE_OK, 1 otherwise.
#include <iostream>
#include <string>
#include <vector>

#include "lsmdb/rpc/clients.h"
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
      opts.sender_node_id = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else {
      args.push_back(a);
    }
  }
  if (args.empty()) return Usage();

  auto channel = MakeChannel(target);
  const std::string& cmd = args[0];

  if (cmd == "put" && args.size() == 3) return Report(KvClient(channel, opts).Put(args[1], args[2]));
  if (cmd == "delete" && args.size() == 2) return Report(KvClient(channel, opts).Delete(args[1]));
  if (cmd == "get" && args.size() == 2) {
    auto r = KvClient(channel, opts).Get(args[1]);
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
    return 0;
  }
  return Usage();
}
