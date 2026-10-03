// Validation decides what storage is ever allowed to see, so the boundaries matter.
#include <gtest/gtest.h>

#include "lsmdb/rpc/status.h"
#include "lsmdb/rpc/validation.h"

using namespace lsmdb;
using namespace lsmdb::rpc;

namespace {
v1::PutRequest MakePut(std::string key, std::string value, std::string request_id = "r1") {
  v1::PutRequest r;
  r.mutable_meta()->set_request_id(std::move(request_id));
  r.set_key(std::move(key));
  r.set_value(std::move(value));
  return r;
}
}  // namespace

TEST(Validation, AcceptsTypicalPut) {
  EXPECT_TRUE(IsOk(ValidatePut(MakePut("k", "v"))));
}

TEST(Validation, RejectsEmptyKey) {
  auto s = ValidatePut(MakePut("", "v"));
  EXPECT_EQ(s.code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_NE(s.message().find("empty"), std::string::npos);
}

TEST(Validation, KeyLengthBoundary) {
  EXPECT_TRUE(IsOk(ValidatePut(MakePut(std::string(kMaxKeyBytes, 'k'), "v"))));
  EXPECT_EQ(ValidatePut(MakePut(std::string(kMaxKeyBytes + 1, 'k'), "v")).code(),
            v1::STATUS_CODE_INVALID_ARGUMENT);
}

TEST(Validation, ValueLengthBoundaryAndEmptyValueAllowed) {
  EXPECT_TRUE(IsOk(ValidatePut(MakePut("k", ""))));  // empty value is legal
  EXPECT_TRUE(IsOk(ValidatePut(MakePut("k", std::string(kMaxValueBytes, 'v')))));
  EXPECT_EQ(ValidatePut(MakePut("k", std::string(kMaxValueBytes + 1, 'v'))).code(),
            v1::STATUS_CODE_INVALID_ARGUMENT);
}

TEST(Validation, RequestIdTooLongRejectedForEveryRequestType) {
  std::string long_id(kMaxRequestIdBytes + 1, 'x');
  EXPECT_EQ(ValidatePut(MakePut("k", "v", long_id)).code(), v1::STATUS_CODE_INVALID_ARGUMENT);

  v1::GetRequest g;
  g.mutable_meta()->set_request_id(long_id);
  g.set_key("k");
  EXPECT_EQ(ValidateGet(g).code(), v1::STATUS_CODE_INVALID_ARGUMENT);

  v1::DeleteRequest d;
  d.mutable_meta()->set_request_id(long_id);
  d.set_key("k");
  EXPECT_EQ(ValidateDelete(d).code(), v1::STATUS_CODE_INVALID_ARGUMENT);
}

TEST(Validation, MissingMetaIsAllowed) {
  v1::GetRequest g;  // no meta at all
  g.set_key("k");
  EXPECT_TRUE(IsOk(ValidateGet(g)));
}

TEST(Validation, GetAndDeleteRejectEmptyKey) {
  EXPECT_EQ(ValidateGet(v1::GetRequest()).code(), v1::STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateDelete(v1::DeleteRequest()).code(), v1::STATUS_CODE_INVALID_ARGUMENT);
}

TEST(Validation, BinaryKeysWithNulBytesAreValid) {
  EXPECT_TRUE(IsOk(ValidatePut(MakePut(std::string("a\0b", 3), "v"))));
}
