#include "lsmdb/reads/consistent_cluster_client.h"

#include <algorithm>
#include <stdexcept>
#include <thread>

#include "lsmdb/rpc/status.h"

namespace lsmdb::reads {

ConsistentClusterClient::ConsistentClusterClient(std::vector<std::string> targets,
                                                 rpc::ClusterClientOptions options)
    : options_(options), targets_(std::move(targets)) {
  if (targets_.empty()) throw std::invalid_argument("ConsistentClusterClient requires at least one target");
}

std::shared_ptr<rpc::KvClient> ConsistentClusterClient::ClientFor(const std::string& target) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& slot = clients_[target];
  if (!slot) slot = std::make_shared<rpc::KvClient>(rpc::MakeChannel(target), options_.client);
  return slot;
}

std::string ConsistentClusterClient::NextTarget(const std::string& current) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = std::find(targets_.begin(), targets_.end(), current);
  if (it == targets_.end() || ++it == targets_.end()) return targets_.front();
  return *it;
}

std::string ConsistentClusterClient::leader_target() const {
  std::lock_guard<std::mutex> lock(mu_);
  return leader_;
}

v1::Status ConsistentClusterClient::Put(const std::string& key, const std::string& value) {
  std::string target;
  std::size_t attempts;
  {
    std::lock_guard<std::mutex> lock(mu_);
    target = leader_.empty() ? targets_.front() : leader_;
    attempts = targets_.size() + options_.max_redirects;
  }
  for (std::size_t i = 0; i < attempts; ++i) {
    auto client = ClientFor(target);
    v1::Status status = client->Put(key, value);
    if (rpc::IsOk(status)) {
      std::lock_guard<std::mutex> lock(mu_);
      leader_ = target;
      return status;
    }
    if (status.code() == v1::STATUS_CODE_NOT_LEADER) {
      if (status.has_leader_hint() && status.leader_hint().port() != 0) {
        std::string hinted = status.leader_hint().host() + ":" + std::to_string(status.leader_hint().port());
        if (!hinted.empty() && hinted != target) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            if (std::find(targets_.begin(), targets_.end(), hinted) == targets_.end()) {
              targets_.push_back(hinted);
            }
          }
          target = hinted;
          continue;
        }
      }
    }
    if (options_.retry_backoff.count() > 0) {
      std::this_thread::sleep_for(options_.retry_backoff);
    }
    target = NextTarget(target);
  }
  return rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE, "cluster unreachable after redirects");
}

v1::Status ConsistentClusterClient::Delete(const std::string& key) {
  std::string target;
  std::size_t attempts;
  {
    std::lock_guard<std::mutex> lock(mu_);
    target = leader_.empty() ? targets_.front() : leader_;
    attempts = targets_.size() + options_.max_redirects;
  }
  for (std::size_t i = 0; i < attempts; ++i) {
    auto client = ClientFor(target);
    v1::Status status = client->Delete(key);
    if (rpc::IsOk(status)) {
      std::lock_guard<std::mutex> lock(mu_);
      leader_ = target;
      return status;
    }
    if (status.code() == v1::STATUS_CODE_NOT_LEADER) {
      if (status.has_leader_hint() && status.leader_hint().port() != 0) {
        std::string hinted = status.leader_hint().host() + ":" + std::to_string(status.leader_hint().port());
        if (!hinted.empty() && hinted != target) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            if (std::find(targets_.begin(), targets_.end(), hinted) == targets_.end()) {
              targets_.push_back(hinted);
            }
          }
          target = hinted;
          continue;
        }
      }
    }
    if (options_.retry_backoff.count() > 0) {
      std::this_thread::sleep_for(options_.retry_backoff);
    }
    target = NextTarget(target);
  }
  return rpc::MakeStatus(v1::STATUS_CODE_UNAVAILABLE, "cluster unreachable after redirects");
}

rpc::KvClient::GetResult ConsistentClusterClient::Get(const std::string& key) {
  std::string target;
  std::size_t attempts;
  {
    std::lock_guard<std::mutex> lock(mu_);
    target = leader_.empty() ? targets_.front() : leader_;
    attempts = targets_.size() + options_.max_redirects;
  }
  rpc::KvClient::GetResult result;
  for (std::size_t i = 0; i < attempts; ++i) {
    auto client = ClientFor(target);
    result = client->Get(key);
    if (rpc::IsOk(result.status) || result.status.code() == v1::STATUS_CODE_NOT_FOUND) {
      std::lock_guard<std::mutex> lock(mu_);
      leader_ = target;
      return result;
    }
    if (result.status.code() == v1::STATUS_CODE_NOT_LEADER) {
      if (result.status.has_leader_hint() && result.status.leader_hint().port() != 0) {
        std::string hinted = result.status.leader_hint().host() + ":" + std::to_string(result.status.leader_hint().port());
        if (!hinted.empty() && hinted != target) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            if (std::find(targets_.begin(), targets_.end(), hinted) == targets_.end()) {
              targets_.push_back(hinted);
            }
          }
          target = hinted;
          continue;
        }
      }
    }
    if (options_.retry_backoff.count() > 0) {
      std::this_thread::sleep_for(options_.retry_backoff);
    }
    target = NextTarget(target);
  }
  return result;
}

}  // namespace lsmdb::reads
