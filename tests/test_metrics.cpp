#include "test_framework.h"
#include "storage/storage_metrics.h"
#include "storage/memtable.h"
#include "storage/wal.h"
#include <filesystem>

using namespace lsm;

TEST_CASE(TestStorageMetricsExportPrometheusSamples) {
    StorageMetrics metrics;
    metrics.ObserveOperation("PUT", 25);
    metrics.ObserveOperation("PUT", 120);
    metrics.ObserveOperation("GET", 5, false);
    metrics.ObserveWalAppend(75);
    metrics.SetMemtableBytes(4096);
    metrics.SetSSTableStats(3, 12288);
    metrics.ObserveCompaction(1000, 700);

    const auto output = metrics.ExportPrometheus();
    ASSERT_TRUE(output.find("storage_operations_total{operation=\"PUT\"} 2") != std::string::npos);
    ASSERT_TRUE(output.find("storage_operation_errors_total{operation=\"GET\"} 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_operation_latency_microseconds_bucket{operation=\"PUT\",le=\"50\"} 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_wal_append_latency_microseconds_count 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_memtable_bytes 4096") != std::string::npos);
    ASSERT_TRUE(output.find("storage_sstable_count 3") != std::string::npos);
    ASSERT_TRUE(output.find("storage_compactions_total 1") != std::string::npos);
}

TEST_CASE(TestMemTableUpdatesMetrics) {
    StorageMetrics metrics;
    MemTable table(&metrics);
    ASSERT_STATUS_OK(table.Put("key", "value"));
    std::string value;
    ASSERT_STATUS_OK(table.Get("key", &value));
    ASSERT_STATUS_OK(table.Delete("key"));
    const auto output = metrics.ExportPrometheus();
    ASSERT_TRUE(output.find("storage_operations_total{operation=\"PUT\"} 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_operations_total{operation=\"GET\"} 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_operation_errors_total{operation=\"GET\"} 0") != std::string::npos);
    ASSERT_TRUE(output.find("storage_operations_total{operation=\"DELETE\"} 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_memtable_bytes 0") != std::string::npos);
}

TEST_CASE(TestWalWriterRecordsAppendLatency) {
    const std::string path = "./test_metrics_wal.log";
    std::filesystem::remove(path);
    StorageMetrics metrics;
    {
        WalWriter writer(path, false, &metrics);
        ASSERT_STATUS_OK(writer.Open());
        ASSERT_STATUS_OK(writer.Append(WalRecord{1, WalOpType::Put, 1, "k", "v"}));
    }
    const auto output = metrics.ExportPrometheus();
    ASSERT_TRUE(output.find("storage_wal_append_latency_microseconds_count 1") != std::string::npos);
    std::filesystem::remove(path);
}
