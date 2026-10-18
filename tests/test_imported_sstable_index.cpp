#include "test_framework.h"

#include "lsmdb/storage/sstable_reader.h"
#include "lsmdb/storage/sstable_writer.h"
#include "lsmdb/storage/version_set.h"
#include "lsmdb/storage/compaction_manager.h"
#include "storage/storage_metrics.h"

#include <filesystem>
#include <cstdio>

using namespace lsmdb::storage;

TEST_CASE(TestSparseIndexIsWrittenAndUsedBySSTableReader) {
    const std::string dir = "./test_indexed_sstable";
    const std::string path = dir + "/table.sst";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        SSTableWriter writer(path);
        for (std::uint64_t i = 0; i < 200; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "key-%06llu", static_cast<unsigned long long>(i));
            if (i == 1) {
                writer.Add(Entry{key, 2, EntryType::kPut, "new"});
                writer.Add(Entry{key, 1, EntryType::kPut, "old"});
            } else {
                writer.Add(Entry{key, i + 1, EntryType::kPut, "value-" + std::to_string(i)});
            }
        }
        const auto metadata = writer.Finish();
        ASSERT_TRUE(metadata.file_size > 0);
    }
    {
        const auto reader = SSTableReader::Open(path);
        ASSERT_TRUE(reader->footer().index_size > 0);
        ASSERT_TRUE(reader->footer().index_offset > 0);
        const auto found = reader->Get("key-000120");
        ASSERT_TRUE(found.has_value());
        ASSERT_EQ(found->value, "value-120");
        const auto old = reader->Get("key-000001", 1);
        ASSERT_TRUE(old.has_value());
        ASSERT_EQ(old->value, "old");
        ASSERT_FALSE(reader->Get("key-999999").has_value());
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE(TestVersionSetUpdatesSSTableMetrics) {
    const std::string dir = "./test_version_metrics";
    std::filesystem::remove_all(dir);
    lsm::StorageMetrics metrics;
    lsmdb::storage::VersionSet versions(dir, &metrics);
    SSTableMetadata meta;
    meta.path = dir + "/placeholder.sst";
    meta.file_size = 8192;
    meta.min_key = "a";
    meta.max_key = "z";
    const auto file = versions.AddFile(0, 1, meta);
    auto output = metrics.ExportPrometheus();
    ASSERT_TRUE(output.find("storage_sstable_count 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_sstable_bytes 8192") != std::string::npos);

    lsmdb::storage::VersionEdit edit;
    edit.removed.push_back(file);
    versions.Apply(edit);
    output = metrics.ExportPrometheus();
    ASSERT_TRUE(output.find("storage_sstable_count 0") != std::string::npos);
    ASSERT_TRUE(output.find("storage_sstable_bytes 0") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE(TestCompactionManagerRecordsActivityMetrics) {
    const std::string dir = "./test_compaction_metrics";
    std::filesystem::remove_all(dir);
    lsm::StorageMetrics metrics;
    lsmdb::storage::VersionSet versions(dir, &metrics);
    for (std::uint64_t i = 0; i < 4; ++i) {
        const auto number = versions.NewFileNumber();
        SSTableWriter writer(versions.FilePath(0, number));
        writer.Add(Entry{"shared", i + 1, EntryType::kPut, "v" + std::to_string(i)});
        versions.AddFile(0, number, writer.Finish());
    }
    lsmdb::storage::CompactionOptions options;
    options.l0_file_trigger = 4;
    options.level1_max_bytes = 1024 * 1024;
    options.target_file_bytes = 1024 * 1024;
    lsmdb::storage::CompactionManager manager(
        versions, options, [] { return kMaxSequence; }, &metrics);
    ASSERT_TRUE(manager.RunOnce().has_value());
    const auto output = metrics.ExportPrometheus();
    ASSERT_TRUE(output.find("storage_compactions_total 1") != std::string::npos);
    ASSERT_TRUE(output.find("storage_compaction_input_bytes_total ") != std::string::npos);
    ASSERT_TRUE(output.find("storage_sstable_count 1") != std::string::npos);
    std::filesystem::remove_all(dir);
}
