#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "lsmdb/node/persistent_backend.h"

namespace {
namespace fs = std::filesystem;

fs::path DetermineDataDir() {
    const char* env_custom = std::getenv("LSMDB_TEST_TEMP");
    if (env_custom && *env_custom) {
        return fs::path(env_custom) / "lsmdb-cli-persistence-smoke";
    }
    const char* temp_env = std::getenv("TEMP");
    if (temp_env && *temp_env) {
        return fs::path(temp_env) / "lsmdb-cli-persistence-smoke";
    }
    const char* tmp_env = std::getenv("TMP");
    if (tmp_env && *tmp_env) {
        return fs::path(tmp_env) / "lsmdb-cli-persistence-smoke";
    }
    return fs::current_path() / "build" / "test-data" / "lsmdb-cli-persistence-smoke";
}
}  // namespace

int main() {
    const fs::path test_dir = DetermineDataDir();
    std::error_code ec;

    try {
        fs::remove_all(test_dir, ec);

        std::cout << "[Step 1] Opening database and writing key-values..." << std::endl;
        {
            auto db = lsmdb::node::PersistentBackend::Open(test_dir);
            db->Put("user:101", "Alice");
            db->Put("user:102", "Bob");
            db->Put("user:103", "Charlie");
            db->Delete("user:102"); // delete Bob
            db->Flush();
            db->store().WaitForIdle();

            if (db->Get("user:101") != std::optional<std::string>("Alice"))
                throw std::runtime_error("Key user:101 mismatch prior to restart");
            if (db->Get("user:102").has_value())
                throw std::runtime_error("Key user:102 should be deleted prior to restart");
        }

        std::cout << "[Step 2] Simulating process restart and verifying persistent recovery..." << std::endl;
        {
            auto db = lsmdb::node::PersistentBackend::Open(test_dir);
            const auto stats = db->recovery_stats();
            std::cout << "  Recovered last_sequence=" << stats.last_sequence
                      << ", records_applied=" << stats.records_applied << std::endl;

            if (db->Get("user:101") != std::optional<std::string>("Alice"))
                throw std::runtime_error("Key user:101 not recovered after restart");
            if (db->Get("user:102").has_value())
                throw std::runtime_error("Key user:102 resurrected after restart");
            if (db->Get("user:103") != std::optional<std::string>("Charlie"))
                throw std::runtime_error("Key user:103 not recovered after restart");

            // Overwrite existing key and append a new key in WAL without explicit flush
            db->Put("user:101", "AliceUpdated");
            db->Put("user:104", "Dave");
        }

        std::cout << "[Step 3] Simulating un-flushed restart (WAL replay recovery)..." << std::endl;
        {
            auto db = lsmdb::node::PersistentBackend::Open(test_dir);
            if (db->Get("user:101") != std::optional<std::string>("AliceUpdated"))
                throw std::runtime_error("WAL replay failed to restore updated value for user:101");
            if (db->Get("user:104") != std::optional<std::string>("Dave"))
                throw std::runtime_error("WAL replay failed to restore unflushed key user:104");
            if (db->Get("user:102").has_value())
                throw std::runtime_error("Tombstoned key returned value after second restart");
        }

        fs::remove_all(test_dir, ec);
        std::cout << "PASS: Persistent storage lifecycle, restart recovery, and WAL replay verified." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        fs::remove_all(test_dir, ec);
        return 1;
    }
}
