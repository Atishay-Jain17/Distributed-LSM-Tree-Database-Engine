#include "storage/storage_metrics.h"

#include <algorithm>
#include <sstream>

namespace lsm {
namespace {
constexpr std::array<std::uint64_t, 7> kBoundsUs{10, 50, 100, 500, 1000, 5000, 10000};
}

void StorageMetrics::Histogram::Observe(std::uint64_t value) {
    ++count;
    sum_us += value;
    for (std::size_t i = 0; i < kBoundsUs.size(); ++i) {
        if (value <= kBoundsUs[i]) ++buckets[i];
    }
}

void StorageMetrics::ObserveOperation(const std::string& operation, std::uint64_t latency_us, bool success) {
    std::lock_guard lock(mutex_);
    ++operation_counts_[operation];
    auto& errors = operation_errors_[operation];
    if (!success) ++errors;
    operation_latency_[operation].Observe(latency_us);
}

void StorageMetrics::ObserveWalAppend(std::uint64_t latency_us) {
    std::lock_guard lock(mutex_);
    wal_latency_.Observe(latency_us);
}

void StorageMetrics::SetMemtableBytes(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    memtable_bytes_ = bytes;
}

void StorageMetrics::SetSSTableStats(std::uint64_t count, std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    sstable_count_ = count;
    sstable_bytes_ = bytes;
}

void StorageMetrics::ObserveCompaction(std::uint64_t input_bytes, std::uint64_t output_bytes) {
    std::lock_guard lock(mutex_);
    ++compactions_;
    compaction_input_bytes_ += input_bytes;
    compaction_output_bytes_ += output_bytes;
}

std::string StorageMetrics::ExportPrometheus() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    auto write_histogram = [&out](const std::string& name, const std::string& labels, const Histogram& h) {
        for (std::size_t i = 0; i < kBoundsUs.size(); ++i)
            out << name << "_bucket{" << labels << "le=\"" << kBoundsUs[i] << "\"} " << h.buckets[i] << '\n';
        out << name << "_bucket{" << labels << "le=\"+Inf\"} " << h.count << '\n';
        const std::string sample_labels = labels.empty() ? "" : "{" + labels.substr(0, labels.size() - 1) + "}";
        out << name << "_sum" << sample_labels << ' ' << h.sum_us << '\n'
            << name << "_count" << sample_labels << ' ' << h.count << '\n';
    };
    for (const auto& [op, count] : operation_counts_) {
        out << "storage_operations_total{operation=\"" << op << "\"} " << count << '\n';
        out << "storage_operation_errors_total{operation=\"" << op << "\"} " << operation_errors_.at(op) << '\n';
        write_histogram("storage_operation_latency_microseconds", "operation=\"" + op + "\",", operation_latency_.at(op));
    }
    write_histogram("storage_wal_append_latency_microseconds", "", wal_latency_);
    out << "storage_memtable_bytes " << memtable_bytes_ << '\n'
        << "storage_sstable_count " << sstable_count_ << '\n'
        << "storage_sstable_bytes " << sstable_bytes_ << '\n'
        << "storage_compactions_total " << compactions_ << '\n'
        << "storage_compaction_input_bytes_total " << compaction_input_bytes_ << '\n'
        << "storage_compaction_output_bytes_total " << compaction_output_bytes_ << '\n';
    return out.str();
}

} // namespace lsm
