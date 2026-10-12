#pragma once

#include <string>
#include <vector>
#include <memory>
#include <random>
#include <cstdint>
#include <cstddef>
#include <iostream>

namespace lsm {

/**
 * @brief High-performance SkipList for key-value storage.
 *
 * Implements a probabilistic alternative to balanced trees, providing O(log N)
 * insertion, lookup, and deletion with sorted in-order traversal.
 */
class SkipList {
public:
    static constexpr int kMaxHeight = 16;
    static constexpr double kBranchingFactor = 0.5;

    struct Node {
        std::string key;
        std::string value;
        int height;
        Node** forward;

        Node(std::string k, std::string v, int h)
            : key(std::move(k)), value(std::move(v)), height(h) {
            forward = new Node*[h];
            for (int i = 0; i < h; ++i) {
                forward[i] = nullptr;
            }
        }

        ~Node() {
            delete[] forward;
        }

        // Non-copyable
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
    };

    class Iterator {
    public:
        explicit Iterator(const Node* node) : current_(node) {}

        [[nodiscard]] bool Valid() const noexcept {
            return current_ != nullptr;
        }

        void Next() {
            if (Valid()) {
                current_ = current_->forward[0];
            }
        }

        [[nodiscard]] const std::string& Key() const {
            return current_->key;
        }

        [[nodiscard]] const std::string& Value() const {
            return current_->value;
        }

        [[nodiscard]] const Node* Current() const noexcept {
            return current_;
        }

    private:
        const Node* current_;
    };

    explicit SkipList(uint32_t seed = 42)
        : current_height_(1),
          entry_count_(0),
          approximate_bytes_(0),
          rng_(seed),
          dist_(0.0, 1.0) {
        head_ = new Node("", "", kMaxHeight);
    }

    ~SkipList() {
        Clear();
        delete head_;
    }

    // Non-copyable, movable
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    SkipList(SkipList&& other) noexcept
        : head_(other.head_),
          current_height_(other.current_height_),
          entry_count_(other.entry_count_),
          approximate_bytes_(other.approximate_bytes_),
          rng_(std::move(other.rng_)),
          dist_(other.dist_) {
        other.head_ = new Node("", "", kMaxHeight);
        other.current_height_ = 1;
        other.entry_count_ = 0;
        other.approximate_bytes_ = 0;
    }

    SkipList& operator=(SkipList&& other) noexcept {
        if (this != &other) {
            Clear();
            delete head_;

            head_ = other.head_;
            current_height_ = other.current_height_;
            entry_count_ = other.entry_count_;
            approximate_bytes_ = other.approximate_bytes_;
            rng_ = std::move(other.rng_);
            dist_ = other.dist_;

            other.head_ = new Node("", "", kMaxHeight);
            other.current_height_ = 1;
            other.entry_count_ = 0;
            other.approximate_bytes_ = 0;
        }
        return *this;
    }

    /**
     * @brief Inserts or updates a key-value pair.
     * @return true if a new node was inserted, false if an existing key was updated.
     */
    bool Insert(const std::string& key, const std::string& value) {
        Node* update[kMaxHeight];
        Node* curr = head_;

        for (int i = current_height_ - 1; i >= 0; --i) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
            update[i] = curr;
        }

        curr = curr->forward[0];

        // Key exists: update value
        if (curr != nullptr && curr->key == key) {
            approximate_bytes_ -= curr->value.size();
            curr->value = value;
            approximate_bytes_ += value.size();
            return false;
        }

        // Key does not exist: create new node
        int new_height = RandomHeight();
        if (new_height > current_height_) {
            for (int i = current_height_; i < new_height; ++i) {
                update[i] = head_;
            }
            current_height_ = new_height;
        }

        auto* new_node = new Node(key, value, new_height);
        for (int i = 0; i < new_height; ++i) {
            new_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = new_node;
        }

        entry_count_++;
        // Accounting: key + value + pointer array overhead + node struct overhead
        approximate_bytes_ += key.size() + value.size() + sizeof(Node) + (new_height * sizeof(Node*));
        return true;
    }

    /**
     * @brief Retrieves the value associated with key.
     */
    bool Lookup(const std::string& key, std::string* value) const {
        const Node* curr = head_;
        for (int i = current_height_ - 1; i >= 0; --i) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
        }

        curr = curr->forward[0];
        if (curr != nullptr && curr->key == key) {
            if (value != nullptr) {
                *value = curr->value;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool Contains(const std::string& key) const {
        return Lookup(key, nullptr);
    }

    /**
     * @brief Deletes a key from the SkipList.
     * @return true if key was found and deleted, false otherwise.
     */
    bool Delete(const std::string& key) {
        Node* update[kMaxHeight];
        Node* curr = head_;

        for (int i = current_height_ - 1; i >= 0; --i) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
            update[i] = curr;
        }

        curr = curr->forward[0];
        if (curr == nullptr || curr->key != key) {
            return false;
        }

        for (int i = 0; i < current_height_; ++i) {
            if (update[i]->forward[i] != curr) {
                break;
            }
            update[i]->forward[i] = curr->forward[i];
        }

        while (current_height_ > 1 && head_->forward[current_height_ - 1] == nullptr) {
            current_height_--;
        }

        approximate_bytes_ -= (curr->key.size() + curr->value.size() + sizeof(Node) + (curr->height * sizeof(Node*)));
        entry_count_--;
        delete curr;
        return true;
    }

    /**
     * @brief Returns an iterator to the first element in sorted order.
     */
    [[nodiscard]] Iterator Begin() const {
        return Iterator(head_->forward[0]);
    }

    /**
     * @brief Finds the first element whose key is >= target.
     */
    [[nodiscard]] Iterator Seek(const std::string& target) const {
        const Node* curr = head_;
        for (int i = current_height_ - 1; i >= 0; --i) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < target) {
                curr = curr->forward[i];
            }
        }
        return Iterator(curr->forward[0]);
    }

    void Clear() {
        Node* curr = head_->forward[0];
        while (curr != nullptr) {
            Node* next = curr->forward[0];
            delete curr;
            curr = next;
        }
        for (int i = 0; i < kMaxHeight; ++i) {
            head_->forward[i] = nullptr;
        }
        current_height_ = 1;
        entry_count_ = 0;
        approximate_bytes_ = 0;
    }

    [[nodiscard]] size_t EntryCount() const noexcept { return entry_count_; }
    [[nodiscard]] size_t ApproximateBytes() const noexcept { return approximate_bytes_; }
    [[nodiscard]] int CurrentHeight() const noexcept { return current_height_; }
    [[nodiscard]] bool Empty() const noexcept { return entry_count_ == 0; }

private:
    int RandomHeight() {
        int height = 1;
        while (height < kMaxHeight && dist_(rng_) < kBranchingFactor) {
            height++;
        }
        return height;
    }

    Node* head_;
    int current_height_;
    size_t entry_count_;
    size_t approximate_bytes_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> dist_;
};

} // namespace lsm
