#include "lsmdb/rpc/clients.h"

#include "lsmdb/rpc/status.h"

namespace lsmdb::rpc {
namespace {

v1::RequestMeta MakeMeta(std::uint32_t sender, std::uint64_t seq) {
  v1::RequestMeta meta;
  meta.set_request_id("n" + std::to_string(sender) + "-" + std::to_string(seq));
  meta.set_sender_node_id(sender);
  meta.set_client_timestamp_ms(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  return meta;
}

void SetDeadline(grpc::ClientContext& ctx, std::chrono::milliseconds d) {
  ctx.set_deadline(std::chrono::system_clock::now() + d);
}

}  // namespace

std::shared_ptr<grpc::Channel> MakeChannel(const std::string& target) {
  return grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
}

// ---------------------------------------------------------------- KvClient

KvClient::KvClient(std::shared_ptr<grpc::Channel> channel, ClientOptions options)
    : stub_(v1::KvService::NewStub(std::move(channel))), options_(options) {}

v1::RequestMeta KvClient::NextMeta() { return MakeMeta(options_.sender_node_id, counter_++); }
void KvClient::Prepare(grpc::ClientContext& ctx) const { SetDeadline(ctx, options_.deadline); }

v1::Status KvClient::Put(const std::string& key, const std::string& value) {
  v1::PutRequest req;
  *req.mutable_meta() = NextMeta();
  req.set_key(key);
  req.set_value(value);
  v1::PutResponse resp;
  grpc::ClientContext ctx;
  Prepare(ctx);
  grpc::Status st = stub_->Put(&ctx, req, &resp);
  return st.ok() ? resp.status() : FromGrpcStatus(st);
}

KvClient::GetResult KvClient::Get(const std::string& key) {
  v1::GetRequest req;
  *req.mutable_meta() = NextMeta();
  req.set_key(key);
  v1::GetResponse resp;
  grpc::ClientContext ctx;
  Prepare(ctx);
  grpc::Status st = stub_->Get(&ctx, req, &resp);
  if (!st.ok()) return {FromGrpcStatus(st), {}};
  GetResult result{resp.status(), {}};
  if (IsOk(result.status)) result.value = resp.value();
  return result;
}

v1::Status KvClient::Delete(const std::string& key) {
  v1::DeleteRequest req;
  *req.mutable_meta() = NextMeta();
  req.set_key(key);
  v1::DeleteResponse resp;
  grpc::ClientContext ctx;
  Prepare(ctx);
  grpc::Status st = stub_->Delete(&ctx, req, &resp);
  return st.ok() ? resp.status() : FromGrpcStatus(st);
}

// -------------------------------------------------------------- NodeClient

NodeClient::NodeClient(std::shared_ptr<grpc::Channel> channel, ClientOptions options)
    : stub_(v1::NodeService::NewStub(std::move(channel))), options_(options) {}

v1::RequestMeta NodeClient::NextMeta() { return MakeMeta(options_.sender_node_id, counter_++); }
void NodeClient::Prepare(grpc::ClientContext& ctx) const { SetDeadline(ctx, options_.deadline); }

NodeClient::PingResult NodeClient::Ping() {
  v1::PingRequest req;
  *req.mutable_meta() = NextMeta();
  v1::PingResponse resp;
  grpc::ClientContext ctx;
  Prepare(ctx);
  grpc::Status st = stub_->Ping(&ctx, req, &resp);
  if (!st.ok()) return {FromGrpcStatus(st), {}};
  return {resp.status(), resp.responder()};
}

NodeClient::InfoResult NodeClient::GetNodeInfo() {
  v1::GetNodeInfoRequest req;
  *req.mutable_meta() = NextMeta();
  v1::GetNodeInfoResponse resp;
  grpc::ClientContext ctx;
  Prepare(ctx);
  grpc::Status st = stub_->GetNodeInfo(&ctx, req, &resp);
  if (!st.ok()) return {FromGrpcStatus(st), {}, {}};
  InfoResult result{resp.status(), resp.self(), {}};
  result.peers.assign(resp.peers().begin(), resp.peers().end());
  return result;
}

}  // namespace lsmdb::rpc
