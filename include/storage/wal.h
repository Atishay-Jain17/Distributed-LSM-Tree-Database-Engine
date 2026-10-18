#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <memory>
#include "common/status.h"
#include "storage/memtable.h"
#include "storage/storage_metrics.h"

namespace lsm {

enum class WalOpType : uint8_t {
    Put = 1,
    Delete = 2
};

struct WalRecord {
    uint64_t sequence_number = 0;
    WalOpType op_type = WalOpType::Put;
    uint64_t version = 0; // Compatible version/timestamp metadata
    std::string key;
    std::string value;

    bool operator==(const WalRecord& other) const {
        return sequence_number == other.sequence_number &&
               op_type == other.op_type &&
               version == other.version &&
               key == other.key &&
               value == other.value;
    }
};

/**
 * @brief Computes a CRC32 checksum over a byte buffer.
 */
uint32_t ComputeCRC32(const uint8_t* data, size_t length);

/**
 * @brief Append-only Write-Ahead Log writer.
 *
 * Encodes records with length prefixes and CRC32 checksums to ensure
 * crash resilience and safe corruption detection.
 */
class WalWriter {
public:
    explicit WalWriter(std::string log_path, bool sync_on_append = true,
                       StorageMetrics* metrics = nullptr);
    ~WalWriter();

    // Non-copyable, movable
    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;
    WalWriter(WalWriter&&) noexcept;
    WalWriter& operator=(WalWriter&&) noexcept;

    Status Open();
    Status Append(const WalRecord& record);
    Status Sync();
    Status Close();

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] const std::string& Path() const noexcept { return log_path_; }

private:
    std::string log_path_;
    bool sync_on_append_;
    StorageMetrics* metrics_ = nullptr; // non-owning
    std::ofstream file_;
    bool is_open_{false};
    uint64_t last_sequence_number_{0};
    bool has_appended_record_{false};
};

/**
 * @brief Reader and Replayer for Write-Ahead Log records.
 *
 * Safely inspects WAL logs, recovers committed entries into a MemTable,
 * tolerates partial/truncated tail writes from abrupt crashes, and detects corruption.
 */
class WalReader {
public:
    explicit WalReader(std::string log_path);
    ~WalReader() = default;

    /**
     * @brief Reads all valid records from the log.
     * @param out_records Destination vector for valid records.
     * @param has_truncated_tail Optional flag set to true if an incomplete tail record was safely ignored.
     * @return Status::Ok if log parsed cleanly or truncated cleanly at tail; Status::Corruption on inner corruption.
     */
    Status ReadAll(std::vector<WalRecord>* out_records, bool* has_truncated_tail = nullptr) const;

    /**
     * @brief Replays all valid records into a fresh MemTable.
     * @param target_memtable Destination MemTable.
     * @param last_sequence_number Output for the highest sequence number recovered.
     * @param has_truncated_tail Output flag indicating if incomplete tail was skipped.
     */
    Status ReplayInto(MemTable* target_memtable,
                      uint64_t* last_sequence_number = nullptr,
                      bool* has_truncated_tail = nullptr) const;

private:
    std::string log_path_;
};

} // namespace lsm
