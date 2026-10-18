#include "test_framework.h"
#include "storage/wal.h"
#include "storage/memtable.h"
#include <filesystem>
#include <fstream>

using namespace lsm;

TEST_CASE(TestWalBasicWriteAndReplay) {
    std::string path = "./test_wal_basic.log";
    std::filesystem::remove(path);

    {
        WalWriter writer(path, true);
        ASSERT_STATUS_OK(writer.Open());

        WalRecord r1{1, WalOpType::Put, 100, "k1", "v1"};
        WalRecord r2{2, WalOpType::Put, 101, "k2", "v2"};
        WalRecord r3{3, WalOpType::Delete, 102, "k1", ""};

        ASSERT_STATUS_OK(writer.Append(r1));
        ASSERT_STATUS_OK(writer.Append(r2));
        ASSERT_STATUS_OK(writer.Append(r3));
        ASSERT_STATUS_OK(writer.Close());
    }

    // Replay into fresh MemTable
    MemTable memtable;
    WalReader reader(path);
    uint64_t last_seq = 0;
    bool truncated = false;
    ASSERT_STATUS_OK(reader.ReplayInto(&memtable, &last_seq, &truncated));

    ASSERT_FALSE(truncated);
    ASSERT_EQ(last_seq, 3);
    ASSERT_EQ(memtable.EntryCount(), 1);

    std::string val;
    ASSERT_STATUS_CODE(memtable.Get("k1", &val), StatusCode::NotFound);
    ASSERT_STATUS_OK(memtable.Get("k2", &val));
    ASSERT_EQ(val, "v2");

    std::filesystem::remove(path);
}

TEST_CASE(TestWalTruncatedTailCrashRecovery) {
    std::string path = "./test_wal_crash.log";
    std::filesystem::remove(path);

    // Write 3 complete records
    {
        WalWriter writer(path, true);
        ASSERT_STATUS_OK(writer.Open());
        writer.Append(WalRecord{1, WalOpType::Put, 1, "k1", "val1"});
        writer.Append(WalRecord{2, WalOpType::Put, 2, "k2", "val2"});
        writer.Append(WalRecord{3, WalOpType::Put, 3, "k3", "val3"});
        writer.Close();
    }

    // Simulate sudden power loss / process crash by appending incomplete bytes to the end
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        uint32_t fake_crc = 0x12345678;
        uint32_t fake_len = 100;
        file.write(reinterpret_cast<const char*>(&fake_crc), sizeof(fake_crc));
        file.write(reinterpret_cast<const char*>(&fake_len), sizeof(fake_len));
        file.write("partial_payload_bytes_cut_short", 25); // Only 25 of 100 bytes written!
        file.close();
    }

    // Reader should recover the 3 valid records and flag truncated tail safely
    MemTable memtable;
    WalReader reader(path);
    uint64_t last_seq = 0;
    bool truncated = false;
    ASSERT_STATUS_OK(reader.ReplayInto(&memtable, &last_seq, &truncated));

    ASSERT_TRUE(truncated);
    ASSERT_EQ(last_seq, 3);
    ASSERT_EQ(memtable.EntryCount(), 3);

    std::string val;
    ASSERT_STATUS_OK(memtable.Get("k1", &val));
    ASSERT_EQ(val, "val1");
    ASSERT_STATUS_OK(memtable.Get("k2", &val));
    ASSERT_EQ(val, "val2");
    ASSERT_STATUS_OK(memtable.Get("k3", &val));
    ASSERT_EQ(val, "val3");

    std::filesystem::remove(path);
}

TEST_CASE(TestWalCorruptionHandling) {
    std::string path = "./test_wal_corrupt.log";
    std::filesystem::remove(path);

    {
        WalWriter writer(path, true);
        ASSERT_STATUS_OK(writer.Open());
        writer.Append(WalRecord{1, WalOpType::Put, 1, "key1", "val1"});
        writer.Append(WalRecord{2, WalOpType::Put, 2, "key2", "val2"});
        writer.Close();
    }

    // Corrupt one byte inside the first record payload
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(15, std::ios::beg); // Inside payload
        char bad_byte = 'X';
        file.write(&bad_byte, 1);
        file.close();
    }

    WalReader reader(path);
    std::vector<WalRecord> records;
    bool truncated = false;
    Status s = reader.ReadAll(&records, &truncated);
    ASSERT_FALSE(s.ok());
    ASSERT_STATUS_CODE(s, StatusCode::Corruption);

    std::filesystem::remove(path);
}

TEST_CASE(TestWalCorruptChecksumOnFinalCompleteRecordIsNotTruncated) {
    const std::string path = "./test_wal_final_corrupt.log";
    std::filesystem::remove(path);
    {
        WalWriter writer(path, true);
        ASSERT_STATUS_OK(writer.Open());
        ASSERT_STATUS_OK(writer.Append(WalRecord{1, WalOpType::Put, 1, "key", "value"}));
        ASSERT_STATUS_OK(writer.Close());
    }
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 0x01;
        file.seekp(0);
        file.write(&byte, 1);
    }
    WalReader reader(path);
    std::vector<WalRecord> records;
    bool truncated = false;
    const Status status = reader.ReadAll(&records, &truncated);
    ASSERT_STATUS_CODE(status, StatusCode::Corruption);
    ASSERT_FALSE(truncated);
    std::filesystem::remove(path);
}

TEST_CASE(TestWalRejectsNonIncreasingSequenceNumbers) {
    const std::string path = "./test_wal_sequence.log";
    std::filesystem::remove(path);
    {
        WalWriter writer(path, true);
        ASSERT_STATUS_OK(writer.Open());
        ASSERT_STATUS_OK(writer.Append(WalRecord{0, WalOpType::Put, 1, "zero", "0"}));
        ASSERT_STATUS_OK(writer.Append(WalRecord{2, WalOpType::Put, 1, "a", "1"}));
        ASSERT_STATUS_CODE(writer.Append(WalRecord{1, WalOpType::Put, 1, "b", "2"}),
                           StatusCode::InvalidArgument);
        ASSERT_STATUS_OK(writer.Append(WalRecord{3, WalOpType::Put, 1, "c", "3"}));
    }
    WalReader reader(path);
    std::vector<WalRecord> records;
    ASSERT_STATUS_OK(reader.ReadAll(&records));
    ASSERT_EQ(records.size(), 3);
    ASSERT_EQ(records.front().sequence_number, 0);
    ASSERT_EQ(records.back().sequence_number, 3);
    std::filesystem::remove(path);
}

TEST_CASE(TestWalEmptyLog) {
    std::string path = "./test_wal_empty.log";
    std::filesystem::remove(path);

    WalReader reader(path);
    std::vector<WalRecord> records;
    bool truncated = false;
    ASSERT_STATUS_OK(reader.ReadAll(&records, &truncated));
    ASSERT_EQ(records.size(), 0);
    ASSERT_FALSE(truncated);
}
