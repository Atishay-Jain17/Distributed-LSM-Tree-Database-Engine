#include "storage/memtable.h"
#include <chrono>

namespace lsm {

MemTable::MemTable(StorageMetrics* metrics)
    : skiplist_(std::make_unique<SkipList>()), metrics_(metrics) {}

Status MemTable::Put(const std::string& key, const std::string& value) {
    const auto start = std::chrono::steady_clock::now();
    auto record = [this, start](bool success) {
        if (metrics_) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            metrics_->ObserveOperation("PUT", static_cast<std::uint64_t>(us), success);
            metrics_->SetMemtableBytes(ApproximateMemoryUsage());
        }
    };
    if (key.empty()) {
        record(false);
        return Status::InvalidArgument("Key cannot be empty");
    }
    skiplist_->Insert(key, value);
    record(true);
    return Status::Ok();
}

Status MemTable::Get(const std::string& key, std::string* value) const {
    const auto start = std::chrono::steady_clock::now();
    auto record = [this, start](bool success) {
        if (metrics_) metrics_->ObserveOperation("GET", static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count()), success);
    };
    if (key.empty()) {
        record(false);
        return Status::InvalidArgument("Key cannot be empty");
    }
    if (value == nullptr) {
        record(false);
        return Status::InvalidArgument("Output value pointer cannot be null");
    }

    if (skiplist_->Lookup(key, value)) {
        record(true);
        return Status::Ok();
    }
    record(false);
    return Status::NotFound("Key not found in MemTable: " + key);
}

Status MemTable::Delete(const std::string& key) {
    const auto start = std::chrono::steady_clock::now();
    auto record = [this, start](bool success) {
        if (metrics_) {
            metrics_->ObserveOperation("DELETE", static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count()), success);
            metrics_->SetMemtableBytes(ApproximateMemoryUsage());
        }
    };
    if (key.empty()) {
        record(false);
        return Status::InvalidArgument("Key cannot be empty");
    }
    if (skiplist_->Delete(key)) {
        record(true);
        return Status::Ok();
    }
    record(false);
    return Status::NotFound("Key not found in MemTable: " + key);
}

bool MemTable::Contains(const std::string& key) const {
    if (key.empty()) {
        return false;
    }
    return skiplist_->Contains(key);
}

SkipList::Iterator MemTable::NewIterator() const {
    return skiplist_->Begin();
}

size_t MemTable::EntryCount() const noexcept {
    return skiplist_->EntryCount();
}

size_t MemTable::ApproximateMemoryUsage() const noexcept {
    return skiplist_->ApproximateBytes();
}

bool MemTable::Empty() const noexcept {
    return skiplist_->Empty();
}

void MemTable::Clear() {
    skiplist_->Clear();
    if (metrics_) metrics_->SetMemtableBytes(0);
}

} // namespace lsm
