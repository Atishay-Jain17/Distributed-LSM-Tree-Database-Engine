#include "storage/wal.h"
#include <cstring>
#include <filesystem>
#include <array>
#include <limits>
#include <chrono>
#include <cerrno>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lsm {

namespace {

Status SyncPathToDisk(const std::string& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Status::IOError("Could not open WAL for durable sync: " + path);
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) return Status::IOError("FlushFileBuffers failed for WAL: " + path);
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return Status::IOError("Could not open WAL for durable sync: " + path + ": " + std::strerror(errno));
    const int result = ::fsync(fd);
    const int saved_errno = errno;
    ::close(fd);
    if (result != 0) return Status::IOError("fsync failed for WAL: " + path + ": " + std::strerror(saved_errno));
#endif
    return Status::Ok();
}

// Lookup table for CRC-32 (IEEE 802.3)
const std::array<uint32_t, 256>& GetCrcTable() {
    static const auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

} // namespace

uint32_t ComputeCRC32(const uint8_t* data, size_t length) {
    const auto& table = GetCrcTable();
    uint32_t crc = 0xFFFFFFFFL;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFL;
}

// ---------------------------------------------------------------------------
// WalWriter
// ---------------------------------------------------------------------------

WalWriter::WalWriter(std::string log_path, bool sync_on_append, StorageMetrics* metrics)
    : log_path_(std::move(log_path)), sync_on_append_(sync_on_append), metrics_(metrics), is_open_(false) {}

WalWriter::~WalWriter() {
    Close();
}

WalWriter::WalWriter(WalWriter&& other) noexcept
    : log_path_(std::move(other.log_path_)),
      sync_on_append_(other.sync_on_append_),
      metrics_(other.metrics_),
      file_(std::move(other.file_)),
            is_open_(other.is_open_),
            last_sequence_number_(other.last_sequence_number_),
            has_appended_record_(other.has_appended_record_) {
    other.is_open_ = false;
}

WalWriter& WalWriter::operator=(WalWriter&& other) noexcept {
    if (this != &other) {
        Close();
        log_path_ = std::move(other.log_path_);
        sync_on_append_ = other.sync_on_append_;
        metrics_ = other.metrics_;
        file_ = std::move(other.file_);
        is_open_ = other.is_open_;
        last_sequence_number_ = other.last_sequence_number_;
        has_appended_record_ = other.has_appended_record_;
        other.is_open_ = false;
    }
    return *this;
}

Status WalWriter::Open() {
    if (is_open_) {
        return Status::Ok();
    }

    std::filesystem::path p(log_path_);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            return Status::IOError("Failed to create parent directory for WAL: " + ec.message());
        }
    }

    file_.open(log_path_, std::ios::binary | std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        return Status::IOError("Could not open WAL file for writing: " + log_path_);
    }

    is_open_ = true;
    return Status::Ok();
}

Status WalWriter::Append(const WalRecord& record) {
    const auto start = std::chrono::steady_clock::now();
    if (!is_open_) {
        return Status::IOError("WAL writer is not open");
    }

    if (record.key.empty() || record.key.size() > std::numeric_limits<uint32_t>::max() ||
        record.value.size() > std::numeric_limits<uint32_t>::max() ||
        (record.op_type != WalOpType::Put && record.op_type != WalOpType::Delete) ||
        (record.op_type == WalOpType::Delete && !record.value.empty())) {
        return Status::InvalidArgument("Invalid WAL record fields");
    }
    if (has_appended_record_ && record.sequence_number <= last_sequence_number_) {
        return Status::InvalidArgument("WAL sequence numbers must increase");
    }
    constexpr size_t kMaxPayload = 64 * 1024 * 1024;
    if (record.key.size() + record.value.size() + 25 > kMaxPayload) {
        return Status::InvalidArgument("WAL record payload exceeds supported limit");
    }

    // Serialize payload
    std::vector<uint8_t> payload;
    uint32_t key_len = static_cast<uint32_t>(record.key.size());
    uint32_t val_len = static_cast<uint32_t>(record.value.size());
    uint8_t op = static_cast<uint8_t>(record.op_type);

    // Payload size: 8 (seq) + 1 (op) + 8 (version) + 4 (klen) + key + 4 (vlen) + val
    size_t payload_size = sizeof(record.sequence_number) + sizeof(op) +
                          sizeof(record.version) + sizeof(key_len) + key_len +
                          sizeof(val_len) + val_len;
    payload.reserve(payload_size);

    auto append_bytes = [&](const void* src, size_t len) {
        const auto* ptr = static_cast<const uint8_t*>(src);
        payload.insert(payload.end(), ptr, ptr + len);
    };

    append_bytes(&record.sequence_number, sizeof(record.sequence_number));
    append_bytes(&op, sizeof(op));
    append_bytes(&record.version, sizeof(record.version));
    append_bytes(&key_len, sizeof(key_len));
    append_bytes(record.key.data(), key_len);
    append_bytes(&val_len, sizeof(val_len));
    append_bytes(record.value.data(), val_len);

    uint32_t checksum = ComputeCRC32(payload.data(), payload.size());
    uint32_t length_prefix = static_cast<uint32_t>(payload.size());

    file_.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    file_.write(reinterpret_cast<const char*>(&length_prefix), sizeof(length_prefix));
    file_.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

    if (!file_) {
        return Status::IOError("Failed to write record to WAL: " + log_path_);
    }
    last_sequence_number_ = record.sequence_number;
    has_appended_record_ = true;

    if (sync_on_append_) {
        file_.flush();
        if (!file_) return Status::IOError("Failed to flush WAL: " + log_path_);
        const Status sync_status = SyncPathToDisk(log_path_);
        if (!sync_status.ok()) return sync_status;
    }

    if (metrics_) metrics_->ObserveWalAppend(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count()));

    return Status::Ok();
}

Status WalWriter::Sync() {
    if (!is_open_) return Status::IOError("WAL writer is not open");
    file_.flush();
    if (!file_) return Status::IOError("Failed to flush WAL: " + log_path_);
    return SyncPathToDisk(log_path_);
}

Status WalWriter::Close() {
    if (is_open_) {
        file_.flush();
        Status sync_status = file_ ? SyncPathToDisk(log_path_)
                                   : Status::IOError("Failed to flush WAL: " + log_path_);
        file_.close();
        is_open_ = false;
        return sync_status;
    }
    return Status::Ok();
}

bool WalWriter::IsOpen() const noexcept {
    return is_open_;
}

// ---------------------------------------------------------------------------
// WalReader
// ---------------------------------------------------------------------------

WalReader::WalReader(std::string log_path) : log_path_(std::move(log_path)) {}

Status WalReader::ReadAll(std::vector<WalRecord>* out_records, bool* has_truncated_tail) const {
    if (out_records == nullptr) {
        return Status::InvalidArgument("out_records pointer cannot be null");
    }
    out_records->clear();
    if (has_truncated_tail != nullptr) {
        *has_truncated_tail = false;
    }

    if (!std::filesystem::exists(log_path_)) {
        return Status::Ok(); // Empty / new log
    }

    std::ifstream file(log_path_, std::ios::binary | std::ios::in);
    if (!file.is_open()) {
        return Status::IOError("Could not open WAL file for reading: " + log_path_);
    }

    while (file.peek() != EOF) {
        uint32_t checksum = 0;
        uint32_t length_prefix = 0;

        file.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
        std::streamsize bytes_read = file.gcount();
        if (bytes_read == 0) {
            break; // Clean EOF
        }
        if (bytes_read < static_cast<std::streamsize>(sizeof(checksum))) {
            // Truncated header
            if (has_truncated_tail != nullptr) *has_truncated_tail = true;
            break;
        }

        file.read(reinterpret_cast<char*>(&length_prefix), sizeof(length_prefix));
        bytes_read = file.gcount();
        if (bytes_read < static_cast<std::streamsize>(sizeof(length_prefix))) {
            // Truncated length prefix
            if (has_truncated_tail != nullptr) *has_truncated_tail = true;
            break;
        }

        // Sanity check: prevent allocating gigabytes on corrupt length prefix
        if (length_prefix > 64 * 1024 * 1024) {
            return Status::Corruption("WAL record payload length exceeds maximum supported limit");
        }

        std::vector<uint8_t> payload(length_prefix);
        file.read(reinterpret_cast<char*>(payload.data()), length_prefix);
        bytes_read = file.gcount();
        if (bytes_read < static_cast<std::streamsize>(length_prefix)) {
            // Truncated payload at tail of WAL
            if (has_truncated_tail != nullptr) *has_truncated_tail = true;
            break;
        }

        uint32_t computed_crc = ComputeCRC32(payload.data(), payload.size());
        if (computed_crc != checksum) {
            return Status::Corruption("WAL CRC32 checksum mismatch");
        }

        // Parse payload
        size_t offset = 0;
        auto read_bytes = [&](void* dst, size_t len) -> bool {
            if (offset + len > payload.size()) return false;
            std::memcpy(dst, payload.data() + offset, len);
            offset += len;
            return true;
        };

        WalRecord record;
        uint8_t op = 0;
        uint32_t key_len = 0;
        uint32_t val_len = 0;

        if (!read_bytes(&record.sequence_number, sizeof(record.sequence_number)) ||
            !read_bytes(&op, sizeof(op)) ||
            !read_bytes(&record.version, sizeof(record.version)) ||
            !read_bytes(&key_len, sizeof(key_len))) {
            return Status::Corruption("Malformed payload metadata in WAL record");
        }

        if (op != static_cast<uint8_t>(WalOpType::Put) &&
            op != static_cast<uint8_t>(WalOpType::Delete)) {
            return Status::Corruption("Unknown WAL record operation type");
        }
        record.op_type = static_cast<WalOpType>(op);

        if (key_len == 0 || offset + key_len > payload.size()) {
            return Status::Corruption("Key length exceeds WAL payload boundary");
        }
        record.key.assign(reinterpret_cast<const char*>(payload.data() + offset), key_len);
        offset += key_len;

        if (!read_bytes(&val_len, sizeof(val_len))) {
            return Status::Corruption("Missing value length in WAL record");
        }
        if (offset + val_len != payload.size()) {
            return Status::Corruption("Value length does not match payload boundary");
        }
        record.value.assign(reinterpret_cast<const char*>(payload.data() + offset), val_len);

        if ((record.op_type == WalOpType::Delete && !record.value.empty()) ||
            (!out_records->empty() && record.sequence_number <= out_records->back().sequence_number)) {
            return Status::Corruption("Invalid WAL operation or non-increasing sequence number");
        }

        out_records->push_back(std::move(record));
    }

    return Status::Ok();
}

Status WalReader::ReplayInto(MemTable* target_memtable,
                             uint64_t* last_sequence_number,
                             bool* has_truncated_tail) const {
    if (target_memtable == nullptr) {
        return Status::InvalidArgument("Target MemTable pointer cannot be null");
    }

    std::vector<WalRecord> records;
    Status s = ReadAll(&records, has_truncated_tail);
    if (!s.ok()) {
        return s;
    }

    uint64_t max_seq = 0;
    for (const auto& rec : records) {
        if (rec.sequence_number > max_seq) {
            max_seq = rec.sequence_number;
        }

        if (rec.op_type == WalOpType::Put) {
            Status put_s = target_memtable->Put(rec.key, rec.value);
            if (!put_s.ok()) return put_s;
        } else if (rec.op_type == WalOpType::Delete) {
            target_memtable->Delete(rec.key);
        }
    }

    if (last_sequence_number != nullptr) {
        *last_sequence_number = max_seq;
    }

    return Status::Ok();
}

} // namespace lsm
