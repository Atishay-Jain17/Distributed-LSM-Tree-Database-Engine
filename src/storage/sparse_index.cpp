#include "storage/sparse_index.h"
#include <algorithm>
#include <stdexcept>

namespace lsm {
namespace {
void WriteU32(std::ostream& os, uint32_t value) {
    char bytes[4];
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>((value >> (8 * i)) & 0xff);
    os.write(bytes, sizeof(bytes));
}
void WriteU64(std::ostream& os, uint64_t value) {
    char bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<char>((value >> (8 * i)) & 0xff);
    os.write(bytes, sizeof(bytes));
}
bool ReadU32(std::istream& is, uint32_t* value) {
    char bytes[4];
    is.read(bytes, sizeof(bytes));
    if (!is) return false;
    *value = 0;
    for (int i = 0; i < 4; ++i) *value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[i])) << (8 * i);
    return true;
}
bool ReadU64(std::istream& is, uint64_t* value) {
    char bytes[8];
    is.read(bytes, sizeof(bytes));
    if (!is) return false;
    *value = 0;
    for (int i = 0; i < 8; ++i) *value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i])) << (8 * i);
    return true;
}
} // namespace

void SparseIndex::AddEntry(std::string key, uint64_t offset, uint32_t size) {
    if (!entries_.empty() && (key <= entries_.back().key || offset <= entries_.back().offset)) {
        throw std::invalid_argument("Sparse index entries must increase by key and file offset");
    }
    entries_.push_back(IndexEntry{std::move(key), offset, size});
}

void SparseIndex::SetKeyRange(std::string min_key, std::string max_key) {
    if (min_key > max_key || (!entries_.empty() &&
        (min_key > entries_.front().key || max_key < entries_.back().key))) {
        throw std::invalid_argument("Sparse index key range is inconsistent with its entries");
    }
    min_key_ = std::move(min_key);
    max_key_ = std::move(max_key);
}

bool SparseIndex::FindCandidateBlock(const std::string& key, IndexEntry* out_entry) const {
    if (entries_.empty()) {
        return false;
    }

    // Binary search for the first entry whose key is strictly greater than target key
    auto it = std::upper_bound(
        entries_.begin(), entries_.end(), key,
        [](const std::string& target, const IndexEntry& entry) {
            return target < entry.key;
        }
    );

    if (it == entries_.begin()) {
        // Target is smaller than the first indexed key; block 0 is candidate
        if (out_entry != nullptr) {
            *out_entry = entries_.front();
        }
        return true;
    }

    // Candidate block is the predecessor
    --it;
    if (out_entry != nullptr) {
        *out_entry = *it;
    }
    return true;
}

bool SparseIndex::MayContainRange(const std::string& key) const {
    if (entries_.empty()) {
        return false;
    }
    if (min_key_.empty() || max_key_.empty()) return true; // unknown bounds: stay conservative
    return key >= min_key_ && key <= max_key_;
}

void SparseIndex::Clear() {
    entries_.clear();
    min_key_.clear();
    max_key_.clear();
}

void SparseIndex::Serialize(std::ostream& os) const {
    uint32_t count = static_cast<uint32_t>(entries_.size());
    WriteU32(os, count);
    uint32_t min_len = static_cast<uint32_t>(min_key_.size());
    uint32_t max_len = static_cast<uint32_t>(max_key_.size());
    WriteU32(os, min_len);
    os.write(min_key_.data(), min_len);
    WriteU32(os, max_len);
    os.write(max_key_.data(), max_len);

    for (const auto& entry : entries_) {
        uint32_t key_len = static_cast<uint32_t>(entry.key.size());
        WriteU32(os, key_len);
        os.write(entry.key.data(), key_len);
        WriteU64(os, entry.offset);
        WriteU32(os, entry.size);
    }
}

Status SparseIndex::Deserialize(std::istream& is) {
    entries_.clear();
    uint32_t count = 0;
    if (!ReadU32(is, &count)) {
        return Status::Corruption("Failed to read sparse index entry count");
    }

    if (count > 1000000) {
        return Status::Corruption("Sparse index entry count exceeds safe threshold");
    }

    uint32_t min_len = 0, max_len = 0;
    if (!ReadU32(is, &min_len) || min_len > 65536) return Status::Corruption("Invalid sparse index minimum key length");
    min_key_.resize(min_len);
    is.read(min_key_.data(), min_len);
    if (!ReadU32(is, &max_len) || max_len > 65536) return Status::Corruption("Invalid sparse index maximum key length");
    max_key_.resize(max_len);
    is.read(max_key_.data(), max_len);
    if (!is || (count > 0 && min_key_ > max_key_)) return Status::Corruption("Invalid sparse index key range");

    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t key_len = 0;
        if (!ReadU32(is, &key_len) || key_len > 65536) {
            return Status::Corruption("Invalid key length in sparse index");
        }

        std::string key(key_len, '\0');
        is.read(key.data(), key_len);

        uint64_t offset = 0;
        uint32_t size = 0;
        if (!ReadU64(is, &offset) || !ReadU32(is, &size)) {
            return Status::Corruption("Truncated sparse index entry payload");
        }
        if (size == 0) return Status::Corruption("Sparse index entry has an empty block");

        if (!entries_.empty() && (key <= entries_.back().key || offset <= entries_.back().offset)) {
            return Status::Corruption("Sparse index entries are not strictly ordered");
        }
        entries_.push_back(IndexEntry{std::move(key), offset, size});
    }

    if (!entries_.empty() && (min_key_ > entries_.front().key || max_key_ < entries_.back().key))
        return Status::Corruption("Sparse index range excludes indexed keys");

    return Status::Ok();
}

} // namespace lsm
