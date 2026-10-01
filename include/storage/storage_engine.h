#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstddef>
#include "common/status.h"
#include "common/config.h"

namespace lsm {

/**
 * @brief Abstract interface defining the Key-Value storage engine contract.
 *
 * This clean abstraction enables single-node baseline storage in Week 1,
 * and will be replaced/extended in Weeks 2-4 with the full LSM-Tree storage engine
 * (SkipList MemTable, WAL, SSTables, Bloom filters, Compaction) without modifying
 * consuming components (RPC, Consensus, Node lifecycle).
 */
class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    // Engine Lifecycle
    virtual Status Open() = 0;
    virtual Status Close() = 0;

    // Core Key-Value API
    /**
     * @brief Inserts or updates the specified key-value pair.
     * @param key Non-empty key string.
     * @param value Associated value string.
     * @return Status::Ok on success, or Status::InvalidArgument if key is empty.
     */
    virtual Status Put(const std::string& key, const std::string& value) = 0;

    /**
     * @brief Retrieves the value associated with the specified key.
     * @param key Non-empty key string.
     * @param value Output pointer to store the retrieved value. Must not be nullptr.
     * @return Status::Ok on success, Status::NotFound if missing, or Status::InvalidArgument.
     */
    virtual Status Get(const std::string& key, std::string* value) const = 0;

    /**
     * @brief Deletes the specified key from the storage engine.
     * @param key Non-empty key string.
     * @return Status::Ok on success, Status::NotFound if missing, or Status::InvalidArgument.
     */
    virtual Status Delete(const std::string& key) = 0;

    // Queries and accounting
    /**
     * @brief Checks if the key exists in the storage engine.
     */
    virtual bool Contains(const std::string& key) const = 0;

    /**
     * @brief Returns the total number of live keys in the storage engine.
     */
    virtual size_t EntryCount() const = 0;

    /**
     * @brief Returns the approximate total size in bytes occupied by keys and values.
     */
    virtual size_t ApproximateSize() const = 0;

    /**
     * @brief Clears all entries from storage.
     */
    virtual void Clear() = 0;
};

} // namespace lsm
