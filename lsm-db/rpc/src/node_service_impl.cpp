#include "lsmdb/rpc/node_service_impl.h"

#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/validation.h"

namespace lsmdb::rpc {

NodeServiceImpl::NodeServiceImpl(v1::NodeInfo self, std::vector<v1::NodeInfo> peers,
                                 std::shared_ptr<consensus::RaftNode> raft)
    : self_(std::move(self)), peers_(std::move(peers)), raft_(std::move(raft)), self_port_(self_.port()) {}

v1::NodeInfo NodeServiceImpl::SelfInfo() const {
  v1::NodeInfo info = self_;
  info.set_port(self_port_.load());
  return info;
}

grpc::Status NodeServiceImpl::Ping(grpc::ServerContext*, const v1::PingRequest* req,
                                   v1::PingResponse* resp) {
  if (auto s = ValidateMeta(req->meta()); !IsOk(s)) {
    *resp->mutable_status() = std::move(s);
    return grpc::Status::OK;
  }
  *resp->mutable_status() = MakeStatus(v1::STATUS_CODE_OK);
  *resp->mutable_responder() = SelfInfo();
  return grpc::Status::OK;
}

grpc::Status NodeServiceImpl::GetNodeInfo(grpc::ServerContext*, const v1::GetNodeInfoRequest* req,
                                          v1::GetNodeInfoResponse* resp) {
  if (auto s = ValidateMeta(req->meta()); !IsOk(s)) {
    *resp->mutable_status() = std::move(s);
    return grpc::Status::OK;
  }
  *resp->mutable_status() = MakeStatus(v1::STATUS_CODE_OK);
  *resp->mutable_self() = SelfInfo();
  for (const auto& peer : peers_) *resp->add_peers() = peer;
  if (raft_) {
    resp->set_is_leader(raft_->IsLeader());
    resp->set_current_term(raft_->CurrentTerm());
    resp->set_leader_id(raft_->LeaderId());
    resp->set_commit_index(raft_->CommitIndex());
    resp->set_last_applied(raft_->LastApplied());
  }
  return grpc::Status::OK;
}

}  // namespace lsmdb::rpc
