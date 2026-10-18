#include "storage/basic_storage_engine.h"

#include <filesystem>
#include <mutex>

namespace lsm {

BasicStorageEngine::BasicStorageEngine(StorageConfig config)
    : config_(std::move(config)), is_open_(false), approximate_bytes_(0) {}

BasicStorageEngine::~BasicStorageEngine() {
    Close();
}

BasicStorageEngine::BasicStorageEngine(BasicStorageEngine&& other) noexcept {
    std::unique_lock<std::shared_mutex> lock(other.mutex_);
    config_ = std::move(other.config_);
    is_open_.store(other.is_open_.load());
    table_ = std::move(other.table_);
    approximate_bytes_ = other.approximate_bytes_;
    other.is_open_.store(false);
    other.approximate_bytes_ = 0;
}

BasicStorageEngine& BasicStorageEngine::operator=(BasicStorageEngine&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        config_ = std::move(other.config_);
        is_open_.store(other.is_open_.load());
        table_ = std::move(other.table_);
        approximate_bytes_ = other.approximate_bytes_;
        other.is_open_.store(false);
        other.approximate_bytes_ = 0;
    }
    return *this;
}

Status BasicStorageEngine::Open() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (is_open_.load()) {
        return Status::Ok();
    }

    if (!config_.data_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(config_.data_dir, ec);
        if (ec) {
            return Status::IOError("Failed to create data directory '" + 
                                   config_.data_dir + "': " + ec.message());
        }
    }

    is_open_.store(true);
    return Status::Ok();
}

Status BasicStorageEngine::Close() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    is_open_.store(false);
    return Status::Ok();
}

Status BasicStorageEngine::Put(const std::string& key, const std::string& value) {
    if (!is_open_.load()) {
        return Status::IOError("Storage engine is not open");
    }
    if (key.empty()) {
        return Status::InvalidArgument("Key cannot be empty");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = table_.find(key);
    if (it != table_.end()) {
        // Overwrite existing key
        approximate_bytes_ -= it->second.size();
        it->second = value;
        approximate_bytes_ += value.size();
    } else {
        // Insert new key
        table_.emplace(key, value);
        approximate_bytes_ += key.size() + value.size();
    }

    return Status::Ok();
}

Status BasicStorageEngine::Get(const std::string& key, std::string* value) const {
    if (!is_open_.load()) {
        return Status::IOError("Storage engine is not open");
    }
    if (key.empty()) {
        return Status::InvalidArgument("Key cannot be empty");
    }
    if (value == nullptr) {
        return Status::InvalidArgument("Output value pointer cannot be null");
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) {
        return Status::NotFound("Key not found: " + key);
    }

    *value = it->second;
    return Status::Ok();
}

Status BasicStorageEngine::Delete(const std::string& key) {
    if (!is_open_.load()) {
        return Status::IOError("Storage engine is not open");
    }
    if (key.empty()) {
        return Status::InvalidArgument("Key cannot be empty");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = table_.find(key);
    if (it == table_.end()) {
        return Status::NotFound("Key not found: " + key);
    }

    approximate_bytes_ -= (it->first.size() + it->second.size());
    table_.erase(it);
    return Status::Ok();
}

bool BasicStorageEngine::Contains(const std::string& key) const {
    if (!is_open_.load() || key.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.find(key) != table_.end();
}

size_t BasicStorageEngine::EntryCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return table_.size();
}

size_t BasicStorageEngine::ApproximateSize() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return approximate_bytes_;
}

void BasicStorageEngine::Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
    approximate_bytes_ = 0;
}

} // namespace lsm
