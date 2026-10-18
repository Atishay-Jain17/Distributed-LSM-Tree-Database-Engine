#pragma once

#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include "storage/storage_engine.h"

namespace lsm {

/**
 * @brief Basic in-memory single-node storage engine for Week 1 baseline.
 *
 * This implementation provides single-node PUT/GET/DELETE semantics,
 * strict status reporting, and size accounting. It satisfies all Week 1
 * requirements while leaving the storage engine interface prepared for the
 * MemTable/SSTable implementations planned for subsequent weeks.
 */
class BasicStorageEngine : public StorageEngine {
public:
    explicit BasicStorageEngine(StorageConfig config = StorageConfig{});
    ~BasicStorageEngine() override;

    // Non-copyable, movable
    BasicStorageEngine(const BasicStorageEngine&) = delete;
    BasicStorageEngine& operator=(const BasicStorageEngine&) = delete;
    BasicStorageEngine(BasicStorageEngine&&) noexcept;
    BasicStorageEngine& operator=(BasicStorageEngine&&) noexcept;

    // StorageEngine Lifecycle
    Status Open() override;
    Status Close() override;

    // Core Key-Value API
    Status Put(const std::string& key, const std::string& value) override;
    Status Get(const std::string& key, std::string* value) const override;
    Status Delete(const std::string& key) override;

    // Introspection
    bool Contains(const std::string& key) const override;
    size_t EntryCount() const override;
    size_t ApproximateSize() const override;
    void Clear() override;

    [[nodiscard]] bool IsOpen() const noexcept { return is_open_.load(); }
    [[nodiscard]] const StorageConfig& GetConfig() const noexcept { return config_; }

private:
    StorageConfig config_;
    std::atomic<bool> is_open_{false};
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> table_;
    size_t approximate_bytes_{0};
};

} // namespace lsm
