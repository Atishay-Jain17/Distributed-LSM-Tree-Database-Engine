#pragma once

#include <string>
#include <memory>
#include <vector>
#include "common/status.h"
#include "storage/skiplist.h"
#include "storage/storage_metrics.h"

namespace lsm {

/**
 * @brief In-memory MemTable backed by a probabilistic SkipList.
 *
 * Provides fast concurrent/single-node writes and ordered reads.
 * Exposes ordered iteration for SSTable flushing and tracks memory consumption
 * for trigger-based flush thresholds.
 */
class MemTable {
public:
    explicit MemTable(StorageMetrics* metrics = nullptr);
    ~MemTable() = default;

    // Non-copyable, movable
    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;
    MemTable(MemTable&&) noexcept = default;
    MemTable& operator=(MemTable&&) noexcept = default;

    /**
     * @brief Inserts or updates an entry in the MemTable.
     */
    Status Put(const std::string& key, const std::string& value);

    /**
     * @brief Retrieves an entry from the MemTable.
     * @return Status::Ok if found, Status::NotFound if absent.
     */
    Status Get(const std::string& key, std::string* value) const;

    /**
     * @brief Removes a key from the MemTable.
     * @return Status::Ok if removed, Status::NotFound if absent.
     */
    Status Delete(const std::string& key);

    [[nodiscard]] bool Contains(const std::string& key) const;

    /**
     * @brief Returns an iterator over the underlying SkipList in lexicographical order.
     */
    [[nodiscard]] SkipList::Iterator NewIterator() const;

    /**
     * @brief Returns the total number of distinct entries currently stored.
     */
    [[nodiscard]] size_t EntryCount() const noexcept;

    /**
     * @brief Returns the approximate memory consumption in bytes.
     */
    [[nodiscard]] size_t ApproximateMemoryUsage() const noexcept;

    [[nodiscard]] bool Empty() const noexcept;

    void Clear();

private:
    std::unique_ptr<SkipList> skiplist_;
    StorageMetrics* metrics_ = nullptr; // non-owning; caller controls collector lifetime
};

} // namespace lsm
