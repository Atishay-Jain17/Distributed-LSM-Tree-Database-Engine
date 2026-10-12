#pragma once

#include <string>
#include <memory>
#include <vector>
#include "common/status.h"
#include "storage/sparse_index.h"

namespace lsm {

/**
 * @brief Minimal SSTable Reader interface contract needed by the indexing layer.
 *
 * Full SSTable serialization, disk block layout, and reader/writer implementation
 * are owned by Vinayak. This interface enables index-assisted lookup testing
 * and multi-file reads without taking over Vinayak's subsystem.
 */
class ISSTableReader {
public:
    virtual ~ISSTableReader() = default;

    virtual Status Get(const std::string& key, std::string* value) const = 0;
    virtual bool MayContain(const std::string& key) const = 0;
    virtual const SparseIndex& GetSparseIndex() const = 0;
    virtual uint64_t TableId() const noexcept = 0;
};

// Searches immutable tables newest-first. A newer tombstone is authoritative
// and prevents an older table's value from being resurrected.
class MultiTableReader {
public:
    explicit MultiTableReader(std::vector<const ISSTableReader*> tables)
        : tables_(std::move(tables)) {}

    Status Get(const std::string& key, std::string* value) const {
        if (value == nullptr) return Status::InvalidArgument("Output value pointer cannot be null");
        for (const auto* table : tables_) {
            if (table == nullptr || !table->MayContain(key)) continue;
            const Status status = table->Get(key, value);
            if (status.ok()) return status;
            if (status.code() != StatusCode::NotFound) return status;
        }
        return Status::NotFound("Key not found in SSTable set: " + key);
    }

private:
    std::vector<const ISSTableReader*> tables_; // newest to oldest
};

/**
 * @brief Test double simulating an SSTable file partitioned into indexed blocks.
 */
class IndexedBlockTableMock : public ISSTableReader {
public:
    struct Block {
        uint64_t offset;
        uint32_t size;
        std::vector<std::pair<std::string, std::string>> records;
    };

    explicit IndexedBlockTableMock(uint64_t table_id) : table_id_(table_id) {}

    void AddBlock(std::string sample_key, uint64_t offset, uint32_t size,
                  std::vector<std::pair<std::string, std::string>> records) {
        index_.AddEntry(std::move(sample_key), offset, size);
        if (!records.empty()) {
            const std::string min_key = blocks_.empty() ? records.front().first : index_.Entries().front().key;
            const std::string max_key = records.back().first;
            index_.SetKeyRange(min_key, max_key);
        }
        blocks_.push_back(Block{offset, size, std::move(records)});
    }

    Status Get(const std::string& key, std::string* value) const override {
        IndexEntry entry;
        if (!index_.FindCandidateBlock(key, &entry)) {
            return Status::NotFound("Key not found in mock SSTable");
        }

        // Locate block by offset
        for (const auto& blk : blocks_) {
            if (blk.offset == entry.offset) {
                for (const auto& rec : blk.records) {
                    if (rec.first == key) {
                        if (value) *value = rec.second;
                        return Status::Ok();
                    }
                }
                break;
            }
        }
        return Status::NotFound("Key not found in candidate block");
    }

    bool MayContain(const std::string& key) const override {
        return index_.MayContainRange(key);
    }

    const SparseIndex& GetSparseIndex() const override {
        return index_;
    }

    uint64_t TableId() const noexcept override {
        return table_id_;
    }

private:
    uint64_t table_id_;
    SparseIndex index_;
    std::vector<Block> blocks_;
};

} // namespace lsm
