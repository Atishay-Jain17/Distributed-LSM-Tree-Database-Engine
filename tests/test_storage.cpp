#include "test_framework.h"
#include "storage/basic_storage_engine.h"

using namespace lsm;

TEST_CASE(TestStorageLifecycle) {
    BasicStorageEngine engine;
    ASSERT_FALSE(engine.IsOpen());

    // Calling operations before Open should return IOError
    std::string val;
    ASSERT_STATUS_CODE(engine.Get("key1", &val), StatusCode::IOError);
    ASSERT_STATUS_CODE(engine.Put("key1", "val1"), StatusCode::IOError);
    ASSERT_STATUS_CODE(engine.Delete("key1"), StatusCode::IOError);

    ASSERT_STATUS_OK(engine.Open());
    ASSERT_TRUE(engine.IsOpen());

    ASSERT_STATUS_OK(engine.Put("key1", "val1"));
    ASSERT_STATUS_OK(engine.Get("key1", &val));
    ASSERT_EQ(val, "val1");

    ASSERT_STATUS_OK(engine.Close());
    ASSERT_FALSE(engine.IsOpen());
    ASSERT_STATUS_CODE(engine.Get("key1", &val), StatusCode::IOError);
}

TEST_CASE(TestPutAndGet) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    ASSERT_STATUS_OK(engine.Put("user:1001", "Alice"));
    ASSERT_STATUS_OK(engine.Put("user:1002", "Bob"));

    std::string val1, val2;
    ASSERT_STATUS_OK(engine.Get("user:1001", &val1));
    ASSERT_STATUS_OK(engine.Get("user:1002", &val2));

    ASSERT_EQ(val1, "Alice");
    ASSERT_EQ(val2, "Bob");
    ASSERT_EQ(engine.EntryCount(), 2);
    ASSERT_TRUE(engine.Contains("user:1001"));
    ASSERT_TRUE(engine.Contains("user:1002"));
}

TEST_CASE(TestOverwrite) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    ASSERT_STATUS_OK(engine.Put("counter", "1"));
    std::string val;
    ASSERT_STATUS_OK(engine.Get("counter", &val));
    ASSERT_EQ(val, "1");

    // Overwrite with a new value
    ASSERT_STATUS_OK(engine.Put("counter", "2"));
    ASSERT_STATUS_OK(engine.Get("counter", &val));
    ASSERT_EQ(val, "2");
    ASSERT_EQ(engine.EntryCount(), 1);

    // Overwrite with longer value and check size
    size_t prev_size = engine.ApproximateSize();
    ASSERT_STATUS_OK(engine.Put("counter", "long_counter_value_string"));
    ASSERT_STATUS_OK(engine.Get("counter", &val));
    ASSERT_EQ(val, "long_counter_value_string");
    ASSERT_TRUE(engine.ApproximateSize() > prev_size);
}

TEST_CASE(TestMissingKey) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    std::string val;
    Status s = engine.Get("non_existent_key", &val);
    ASSERT_FALSE(s.ok());
    ASSERT_STATUS_CODE(s, StatusCode::NotFound);
    ASSERT_FALSE(engine.Contains("non_existent_key"));
}

TEST_CASE(TestDelete) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    ASSERT_STATUS_OK(engine.Put("temp_key", "temporary_value"));
    ASSERT_TRUE(engine.Contains("temp_key"));
    ASSERT_EQ(engine.EntryCount(), 1);

    ASSERT_STATUS_OK(engine.Delete("temp_key"));
    ASSERT_FALSE(engine.Contains("temp_key"));
    ASSERT_EQ(engine.EntryCount(), 0);
    ASSERT_EQ(engine.ApproximateSize(), 0);

    std::string val;
    ASSERT_STATUS_CODE(engine.Get("temp_key", &val), StatusCode::NotFound);
}

TEST_CASE(TestDeleteMissingKey) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    Status s = engine.Delete("key_that_does_not_exist");
    ASSERT_FALSE(s.ok());
    ASSERT_STATUS_CODE(s, StatusCode::NotFound);
}

TEST_CASE(TestInputValidation) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    // Empty key validation
    ASSERT_STATUS_CODE(engine.Put("", "value"), StatusCode::InvalidArgument);
    std::string val;
    ASSERT_STATUS_CODE(engine.Get("", &val), StatusCode::InvalidArgument);
    ASSERT_STATUS_CODE(engine.Delete(""), StatusCode::InvalidArgument);
    ASSERT_FALSE(engine.Contains(""));

    // Null output pointer validation
    ASSERT_STATUS_CODE(engine.Get("valid_key", nullptr), StatusCode::InvalidArgument);
}

TEST_CASE(TestClear) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    for (int i = 0; i < 50; ++i) {
        ASSERT_STATUS_OK(engine.Put("key_" + std::to_string(i), "val_" + std::to_string(i)));
    }
    ASSERT_EQ(engine.EntryCount(), 50);
    ASSERT_TRUE(engine.ApproximateSize() > 0);

    engine.Clear();
    ASSERT_EQ(engine.EntryCount(), 0);
    ASSERT_EQ(engine.ApproximateSize(), 0);
    ASSERT_FALSE(engine.Contains("key_0"));
}

TEST_CASE(TestMultipleKeysIsolation) {
    BasicStorageEngine engine;
    ASSERT_STATUS_OK(engine.Open());

    constexpr int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        std::string key = "key#" + std::to_string(i);
        std::string val = "value#" + std::to_string(i * 10);
        ASSERT_STATUS_OK(engine.Put(key, val));
    }

    ASSERT_EQ(engine.EntryCount(), kCount);

    for (int i = 0; i < kCount; ++i) {
        std::string key = "key#" + std::to_string(i);
        std::string expected_val = "value#" + std::to_string(i * 10);
        std::string actual_val;
        ASSERT_STATUS_OK(engine.Get(key, &actual_val));
        ASSERT_EQ(actual_val, expected_val);
    }
}
