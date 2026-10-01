#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <filesystem>
#include "storage/basic_storage_engine.h"
#include "common/config.h"

using namespace lsm;

int main() {
    std::cout << "======================================================" << std::endl;
    std::cout << "  Starting Week 1 Storage Smoke Test (Single Node)    " << std::endl;
    std::cout << "======================================================" << std::endl;

    std::string test_dir = "./smoke_test_data";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);

    StorageConfig config;
    config.data_dir = test_dir;
    config.sync_wal = true;

    BasicStorageEngine engine(config);

    auto start_time = std::chrono::steady_clock::now();

    // 1. Open
    std::cout << "[Step 1] Opening storage engine at " << config.data_dir << " ... ";
    Status s = engine.Open();
    if (!s.ok()) {
        std::cerr << "FAILED: " << s.ToString() << std::endl;
        return 1;
    }
    std::cout << "OK" << std::endl;

    // 2. Sequential Writes
    constexpr int kRecords = 500;
    std::cout << "[Step 2] Performing " << kRecords << " sequential PUT operations ... ";
    for (int i = 0; i < kRecords; ++i) {
        std::string key = "key_smk_" + std::to_string(i);
        std::string val = "value_smk_" + std::to_string(i);
        s = engine.Put(key, val);
        if (!s.ok()) {
            std::cerr << "FAILED at key " << key << ": " << s.ToString() << std::endl;
            return 1;
        }
    }
    if (engine.EntryCount() != kRecords) {
        std::cerr << "FAILED: EntryCount mismatch. Expected " << kRecords 
                  << ", got " << engine.EntryCount() << std::endl;
        return 1;
    }
    std::cout << "OK (Entries: " << engine.EntryCount() << ", ApproxSize: " << engine.ApproximateSize() << " bytes)" << std::endl;

    // 3. Sequential Reads
    std::cout << "[Step 3] Verifying " << kRecords << " sequential GET operations ... ";
    for (int i = 0; i < kRecords; ++i) {
        std::string key = "key_smk_" + std::to_string(i);
        std::string expected_val = "value_smk_" + std::to_string(i);
        std::string actual_val;
        s = engine.Get(key, &actual_val);
        if (!s.ok() || actual_val != expected_val) {
            std::cerr << "FAILED at key " << key << ": expected " << expected_val 
                      << ", got " << actual_val << " (" << s.ToString() << ")" << std::endl;
            return 1;
        }
    }
    std::cout << "OK" << std::endl;

    // 4. Overwrite half of records
    constexpr int kOverwrites = 250;
    std::cout << "[Step 4] Overwriting " << kOverwrites << " keys with new values ... ";
    for (int i = 0; i < kOverwrites; ++i) {
        std::string key = "key_smk_" + std::to_string(i);
        std::string new_val = "updated_val_" + std::to_string(i);
        s = engine.Put(key, new_val);
        if (!s.ok()) {
            std::cerr << "FAILED overwrite at key " << key << ": " << s.ToString() << std::endl;
            return 1;
        }
    }
    for (int i = 0; i < kOverwrites; ++i) {
        std::string key = "key_smk_" + std::to_string(i);
        std::string expected_val = "updated_val_" + std::to_string(i);
        std::string actual_val;
        s = engine.Get(key, &actual_val);
        if (!s.ok() || actual_val != expected_val) {
            std::cerr << "FAILED verify overwrite at key " << key << std::endl;
            return 1;
        }
    }
    std::cout << "OK" << std::endl;

    // 5. Deletes
    constexpr int kDeletes = 100;
    std::cout << "[Step 5] Deleting " << kDeletes << " keys ... ";
    for (int i = 0; i < kDeletes; ++i) {
        std::string key = "key_smk_" + std::to_string(i);
        s = engine.Delete(key);
        if (!s.ok()) {
            std::cerr << "FAILED delete at key " << key << ": " << s.ToString() << std::endl;
            return 1;
        }
    }
    if (engine.EntryCount() != static_cast<size_t>(kRecords - kDeletes)) {
        std::cerr << "FAILED: EntryCount after deletes is " << engine.EntryCount() 
                  << ", expected " << (kRecords - kDeletes) << std::endl;
        return 1;
    }
    // Verify deleted keys return NotFound
    for (int i = 0; i < kDeletes; ++i) {
        std::string key = "key_smk_" + std::to_string(i);
        std::string val;
        s = engine.Get(key, &val);
        if (s.code() != StatusCode::NotFound) {
            std::cerr << "FAILED: Deleted key " << key << " still returned status " << s.ToString() << std::endl;
            return 1;
        }
    }
    std::cout << "OK (Remaining: " << engine.EntryCount() << ")" << std::endl;

    // 6. Error & Boundary Handling
    std::cout << "[Step 6] Testing edge cases (empty key, missing key, null pointer) ... ";
    {
        std::string dummy;
        if (engine.Get("", &dummy).code() != StatusCode::InvalidArgument ||
            engine.Put("", "val").code() != StatusCode::InvalidArgument ||
            engine.Delete("").code() != StatusCode::InvalidArgument ||
            engine.Get("valid_key", nullptr).code() != StatusCode::InvalidArgument ||
            engine.Get("missing_key_9999", &dummy).code() != StatusCode::NotFound ||
            engine.Delete("missing_key_9999").code() != StatusCode::NotFound) {
            std::cerr << "FAILED edge cases validation" << std::endl;
            return 1;
        }
    }
    std::cout << "OK" << std::endl;

    // 7. Close and Cleanup
    std::cout << "[Step 7] Closing storage engine ... ";
    s = engine.Close();
    if (!s.ok()) {
        std::cerr << "FAILED close: " << s.ToString() << std::endl;
        return 1;
    }
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "OK" << std::endl;

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "======================================================" << std::endl;
    std::cout << "  Smoke Test Passed Successfully in " << elapsed_ms << " ms! " << std::endl;
    std::cout << "======================================================" << std::endl;

    return 0;
}
