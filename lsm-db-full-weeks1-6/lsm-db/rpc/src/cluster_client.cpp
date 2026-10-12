#include "lsmdb/rpc/cluster_client.h"

#include <algorithm>
#include <thread>

#include "lsmdb/rpc/status.h"

namespace lsmdb::rpc {

ClusterClient::ClusterClient(std::vector<std::string> targets, ClusterClientOptions options)
    : options_(options), targets_(std::move(targets)) {}

std::shared_ptr<KvClient> ClusterClient::ClientFor(const std::string& target) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& slot = clients_[target];
  if (!slot) slot = std::make_shared<KvClient>(MakeChannel(target), options_.client);
  return slot;
}

std::string ClusterClient::NextTarget(const std::string& current) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = std::find(targets_.begin(), targets_.end(), current);
  if (it == targets_.end() || ++it == targets_.end()) it = targets_.begin();
  return *it;
}

std::string ClusterClient::leader_target() const {
  std::lock_guard<std::mutex> lock(mu_);
  return leader_;
}

template <typename Op>
v1::Status ClusterClient::Write(Op op) {
  std::string target;
  std::size_t attempts;
  {
    std::lock_guard<std::mutex> lock(mu_);
    target = leader_.empty() ? targets_.front() : leader_;
    attempts = targets_.size() + options_.max_redirects;
  }
  v1::Status last = MakeStatus(v1::STATUS_CODE_UNAVAILABLE, "no attempt made");
  for (std::size_t i = 0; i < attempts; ++i) {
    last = op(*ClientFor(target));
    if (last.code() == v1::STATUS_CODE_NOT_LEADER) {
      std::string hinted;
      if (last.has_leader_hint() && last.leader_hint().port() != 0) {
        hinted = last.leader_hint().host() + ":" + std::to_string(last.leader_hint().port());
      }
      if (!hinted.empty() && hinted != target) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (std::find(targets_.begin(), targets_.end(), hinted) == targets_.end()) targets_.push_back(hinted);
        }
        target = hinted;  // follow the redirect immediately
        continue;
      }
    } else if (last.code() != v1::STATUS_CODE_UNAVAILABLE) {
      if (IsOk(last)) {
        std::lock_guard<std::mutex> lock(mu_);
        leader_ = target;
      }
      return last;  // OK or a definitive error (INVALID_ARGUMENT, INTERNAL)
    }
    std::this_thread::sleep_for(options_.retry_backoff);
    target = NextTarget(target);
  }
  return last;
}

v1::Status ClusterClient::Put(const std::string& key, const std::string& value) {
  return Write([&](KvClient& c) { return c.Put(key, value); });
}

v1::Status ClusterClient::Delete(const std::string& key) {
  return Write([&](KvClient& c) { return c.Delete(key); });
}

KvClient::GetResult ClusterClient::Get(const std::string& key) {
  std::string target;
  {
    std::lock_guard<std::mutex> lock(mu_);
    target = leader_.empty() ? targets_.front() : leader_;
  }
  KvClient::GetResult result;
  std::size_t count;
  {
    std::lock_guard<std::mutex> lock(mu_);
    count = targets_.size();
  }
  for (std::size_t i = 0; i < count; ++i) {
    result = ClientFor(target)->Get(key);
    if (result.status.code() != v1::STATUS_CODE_UNAVAILABLE) return result;
    target = NextTarget(target);
  }
  return result;
}

}  // namespace lsmdb::rpc
