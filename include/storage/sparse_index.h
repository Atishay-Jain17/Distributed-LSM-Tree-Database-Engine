#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <optional>
#include "common/status.h"

namespace lsm {

/**
 * @brief An entry in the sparse index representing an SSTable block boundary.
 */
struct IndexEntry {
    std::string key;      // Sample key at the start of the block
    uint64_t offset = 0;  // Byte offset of the block in the SSTable file
    uint32_t size = 0;    // Size of the block in bytes

    bool operator==(const IndexEntry& other) const {
        return key == other.key && offset == other.offset && size == other.size;
    }
};

/**
 * @brief In-memory sparse index mapping key intervals to physical file offsets.
 *
 * Implements binary search to quickly locate candidate SSTable blocks without
 * scanning through the entire file.
 */
class SparseIndex {
public:
    SparseIndex() = default;
    ~SparseIndex() = default;

    /**
     * @brief Adds an entry to the index. Entries must be added in ascending key order.
     */
    void AddEntry(std::string key, uint64_t offset, uint32_t size);
    void SetKeyRange(std::string min_key, std::string max_key);

    /**
     * @brief Finds the candidate block that may contain target key.
     *
     * Returns the entry with the greatest key <= target key.
     * If target key is smaller than the first index entry, returns the first block
     * (as keys before the first sample might still reside in block 0).
     * Returns false if the index is empty.
     */
    bool FindCandidateBlock(const std::string& key, IndexEntry* out_entry) const;

    /**
     * @brief Checks if a key could fall within the known table range.
     * Returns true conservatively until SetKeyRange supplies exact bounds.
     */
    [[nodiscard]] bool MayContainRange(const std::string& key) const;

    [[nodiscard]] size_t EntryCount() const noexcept { return entries_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const std::vector<IndexEntry>& Entries() const noexcept { return entries_; }
    [[nodiscard]] const std::string& MinKey() const noexcept { return min_key_; }
    [[nodiscard]] const std::string& MaxKey() const noexcept { return max_key_; }

    void Clear();

    // Serialization for SSTable footer/index block
    void Serialize(std::ostream& os) const;
    Status Deserialize(std::istream& is);

private:
    std::vector<IndexEntry> entries_;
    std::string min_key_;
    std::string max_key_;
};

} // namespace lsm
