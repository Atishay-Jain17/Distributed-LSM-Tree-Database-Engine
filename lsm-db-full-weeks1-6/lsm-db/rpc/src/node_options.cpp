#include "lsmdb/rpc/node_options.h"

#include <set>
#include <stdexcept>

namespace lsmdb::rpc {
namespace {

std::string Trim(const std::string& s) {
  const char* ws = " \t\r\n";
  auto b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

std::uint32_t ParseUint(const std::string& text, std::uint32_t min, std::uint32_t max,
                        const std::string& what) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos ||
      text.size() > 9) {
    throw std::invalid_argument(what + " must be a number, got '" + text + "'");
  }
  std::uint32_t v = static_cast<std::uint32_t>(std::stoul(text));
  if (v < min || v > max) {
    throw std::invalid_argument(what + " out of range [" + std::to_string(min) + ", " +
                                std::to_string(max) + "]: " + text);
  }
  return v;
}

}  // namespace

std::vector<v1::NodeInfo> ParsePeers(const std::string& spec) {
  std::vector<v1::NodeInfo> peers;
  std::set<std::uint32_t> seen;
  if (Trim(spec).empty()) return peers;

  std::size_t start = 0;
  while (start <= spec.size()) {
    std::size_t comma = spec.find(',', start);
    std::string item = Trim(spec.substr(start, comma == std::string::npos ? comma : comma - start));
    auto eq = item.find('=');
    auto colon = item.rfind(':');
    if (eq == std::string::npos || colon == std::string::npos || colon < eq) {
      throw std::invalid_argument("peer must look like id=host:port, got '" + item + "'");
    }
    v1::NodeInfo peer;
    peer.set_node_id(ParseUint(Trim(item.substr(0, eq)), 1, 1000000, "peer id"));
    std::string host = Trim(item.substr(eq + 1, colon - eq - 1));
    if (host.empty()) throw std::invalid_argument("peer host is empty in '" + item + "'");
    peer.set_host(host);
    peer.set_port(ParseUint(Trim(item.substr(colon + 1)), 1, 65535, "peer port"));
    if (!seen.insert(peer.node_id()).second) {
      throw std::invalid_argument("duplicate peer id " + std::to_string(peer.node_id()));
    }
    peers.push_back(std::move(peer));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return peers;
}

void ValidateNodeOptions(const NodeOptions& o, bool allow_ephemeral_port) {
  if (o.node_id == 0) throw std::invalid_argument("node_id must be >= 1");
  if (o.advertise_host.empty()) throw std::invalid_argument("advertise_host must not be empty");
  if (o.port > 65535 || (o.port == 0 && !allow_ephemeral_port)) {
    throw std::invalid_argument("port must be in [1, 65535]");
  }
  std::set<std::uint32_t> seen;
  for (const auto& p : o.peers) {
    if (p.node_id() == o.node_id) throw std::invalid_argument("peers must not include this node");
    if (!seen.insert(p.node_id()).second) {
      throw std::invalid_argument("duplicate peer id " + std::to_string(p.node_id()));
    }
  }
}

}  // namespace lsmdb::rpc
