#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "common/status.h"
#include "storage/storage_engine.h"

namespace lsm {

struct LogEntry {
    uint64_t index = 0;
    uint64_t term = 0;
    std::string command_type; // "PUT" or "DELETE"
    std::string key;
    std::string value;

    bool operator==(const LogEntry& other) const {
        return index == other.index &&
               term == other.term &&
               command_type == other.command_type &&
               key == other.key &&
               value == other.value;
    }
};

struct AppendEntriesArgs {
    uint64_t term = 0;
    std::string leader_id;
    uint64_t prev_log_index = 0;
    uint64_t prev_log_term = 0;
    std::vector<LogEntry> entries;
    uint64_t leader_commit = 0;
};

struct AppendEntriesReply {
    uint64_t term = 0;
    bool success = false;
    uint64_t match_index = 0;
    uint64_t conflict_index = 0;
    uint64_t conflict_term = 0;
};

/**
 * @brief Manages the Raft replicated log, log consistency invariants,
 * follower conflict resolution, and ordered state machine application.
 */
class RaftLog {
public:
    RaftLog();
    ~RaftLog() = default;

    // Log Inspection
    [[nodiscard]] uint64_t LastLogIndex() const noexcept;
    [[nodiscard]] uint64_t LastLogTerm() const noexcept;
    [[nodiscard]] uint64_t CommitIndex() const noexcept { return commit_index_; }
    [[nodiscard]] uint64_t LastApplied() const noexcept { return last_applied_; }
    [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] const LogEntry* GetEntry(uint64_t index) const;
    [[nodiscard]] uint64_t TermAt(uint64_t index) const;
    [[nodiscard]] std::vector<LogEntry> GetEntriesFrom(uint64_t start_index) const;

    // Leader Append
    uint64_t AppendLeaderEntry(uint64_t term, std::string command_type,
                               std::string key, std::string value);

    // Follower Append & Consistency (§5.3)
    /**
     * @brief Evaluates AppendEntries consistency check and applies entries or resolves conflicts.
     * @param prev_log_index Index of log entry immediately preceding new ones.
     * @param prev_log_term Term of prev_log_index entry.
     * @param new_entries Entries to append.
     * @param reply Output reply containing success or conflict indices for retries.
     * @return true if previous-log matched and entries were appended; false on inconsistency.
     */
    bool HandleAppendEntries(uint64_t current_term,
                             const AppendEntriesArgs& args,
                             AppendEntriesReply* reply);

    // Ordered Application
    /**
     * @brief Applies all newly committed log entries up to min(leader_commit, LastLogIndex())
     * to the underlying storage engine in strict log order.
     * @return Number of entries newly applied to storage.
     */
    size_t ApplyCommittedEntries(uint64_t leader_commit, StorageEngine* storage);

private:
    std::vector<LogEntry> entries_; // 1-based log indexing: entries_[0] has index 1
    uint64_t commit_index_{0};
    uint64_t last_applied_{0};
};

} // namespace lsm
