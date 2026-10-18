#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace lsm {

// Small dependency-free metrics collector. Histogram buckets are cumulative,
// so the output can be scraped directly by Prometheus-compatible tools.
class StorageMetrics {
public:
    void ObserveOperation(const std::string& operation, std::uint64_t latency_us, bool success = true);
    void ObserveWalAppend(std::uint64_t latency_us);
    void SetMemtableBytes(std::uint64_t bytes);
    void SetSSTableStats(std::uint64_t count, std::uint64_t bytes);
    void ObserveCompaction(std::uint64_t input_bytes, std::uint64_t output_bytes);
    [[nodiscard]] std::string ExportPrometheus() const;

private:
    struct Histogram {
        std::array<std::uint64_t, 7> buckets{};
        std::uint64_t count = 0;
        std::uint64_t sum_us = 0;
        void Observe(std::uint64_t value);
    };
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> operation_counts_;
    std::map<std::string, std::uint64_t> operation_errors_;
    std::map<std::string, Histogram> operation_latency_;
    Histogram wal_latency_;
    std::uint64_t memtable_bytes_ = 0;
    std::uint64_t sstable_count_ = 0;
    std::uint64_t sstable_bytes_ = 0;
    std::uint64_t compactions_ = 0;
    std::uint64_t compaction_input_bytes_ = 0;
    std::uint64_t compaction_output_bytes_ = 0;
};

} // namespace lsm
