#include "consensus/raft_log.h"
#include <algorithm>

namespace lsm {

RaftLog::RaftLog() = default;

uint64_t RaftLog::LastLogIndex() const noexcept {
    if (entries_.empty()) {
        return 0;
    }
    return entries_.back().index;
}

uint64_t RaftLog::LastLogTerm() const noexcept {
    if (entries_.empty()) {
        return 0;
    }
    return entries_.back().term;
}

const LogEntry* RaftLog::GetEntry(uint64_t index) const {
    if (index == 0 || index > LastLogIndex()) {
        return nullptr;
    }
    return &entries_[index - 1];
}

uint64_t RaftLog::TermAt(uint64_t index) const {
    if (index == 0) {
        return 0;
    }
    if (index > LastLogIndex()) {
        return 0;
    }
    return entries_[index - 1].term;
}

std::vector<LogEntry> RaftLog::GetEntriesFrom(uint64_t start_index) const {
    std::vector<LogEntry> result;
    if (start_index == 0 || start_index > LastLogIndex()) {
        return result;
    }
    result.assign(entries_.begin() + (start_index - 1), entries_.end());
    return result;
}

uint64_t RaftLog::AppendLeaderEntry(uint64_t term, std::string command_type,
                                    std::string key, std::string value) {
    if ((command_type != "PUT" && command_type != "DELETE") || key.empty() ||
        (command_type == "DELETE" && !value.empty())) {
        throw std::invalid_argument("Raft log entry must be a valid PUT or DELETE command");
    }
    uint64_t new_index = LastLogIndex() + 1;
    entries_.push_back(LogEntry{
        new_index,
        term,
        std::move(command_type),
        std::move(key),
        std::move(value)
    });
    return new_index;
}

bool RaftLog::HandleAppendEntries(uint64_t current_term,
                                  const AppendEntriesArgs& args,
                                  AppendEntriesReply* reply) {
    if (reply == nullptr) return false;

    reply->term = current_term;
    reply->success = false;
    reply->match_index = 0;
    reply->conflict_index = 0;
    reply->conflict_term = 0;

    // 1. Reply false if leader's term is smaller than follower's term (§5.1)
    if (args.term < current_term) {
        return false;
    }

    // 2. Reply false if log doesn't contain an entry at prev_log_index
    //    whose term matches prev_log_term (§5.3)
    if (args.prev_log_index > LastLogIndex()) {
        reply->conflict_index = LastLogIndex() + 1;
        reply->conflict_term = 0;
        return false;
    }

    if (args.prev_log_index > 0) {
        uint64_t actual_term = TermAt(args.prev_log_index);
        if (actual_term != args.prev_log_term) {
            reply->conflict_term = actual_term;
            // Identify first index with the conflicting term to accelerate leader retry
            uint64_t first_idx = args.prev_log_index;
            while (first_idx > 1 && TermAt(first_idx - 1) == actual_term) {
                first_idx--;
            }
            reply->conflict_index = first_idx;

            // A failed consistency check must not mutate the follower log. The
            // leader retries from conflict_index; only an actual conflicting
            // incoming entry is allowed to truncate uncommitted suffix data.
            return false;
        }
    }

    // 3. If an existing entry conflicts with a new one (same index, different term),
    //    delete existing entry and all that follow it (§5.3)
    // Validate the RPC's indices before changing any state.
    for (size_t i = 0; i < args.entries.size(); ++i) {
        const auto& entry = args.entries[i];
        if (entry.index != args.prev_log_index + i + 1 ||
            (entry.command_type != "PUT" && entry.command_type != "DELETE") ||
            entry.key.empty() || (entry.command_type == "DELETE" && !entry.value.empty())) return false;
    }
    for (size_t i = 0; i < args.entries.size(); ++i) {
        const auto& new_entry = args.entries[i];
        uint64_t entry_index = new_entry.index;

        if (entry_index <= LastLogIndex()) {
            if (TermAt(entry_index) != new_entry.term) {
                if (entry_index <= commit_index_) return false;
                entries_.resize(entry_index - 1);
                // Append remaining new entries
                for (size_t j = i; j < args.entries.size(); ++j) {
                    entries_.push_back(args.entries[j]);
                }
                break;
            }
        } else {
            entries_.push_back(new_entry);
        }
    }

    reply->success = true;
    reply->match_index = args.prev_log_index + args.entries.size();
    if (args.leader_commit > commit_index_) commit_index_ = std::min(args.leader_commit, LastLogIndex());
    return true;
}

size_t RaftLog::ApplyCommittedEntries(uint64_t leader_commit, StorageEngine* storage) {
    if (leader_commit > commit_index_) {
        commit_index_ = std::min(leader_commit, LastLogIndex());
    }

    size_t applied_count = 0;
    while (last_applied_ < commit_index_) {
        last_applied_++;
        const auto& entry = entries_[last_applied_ - 1];
        if (storage != nullptr) {
            if (entry.command_type == "PUT") {
                const Status status = storage->Put(entry.key, entry.value);
                if (!status.ok()) {
                    --last_applied_;
                    break;
                }
            } else if (entry.command_type == "DELETE") {
                const Status status = storage->Delete(entry.key);
                if (!status.ok() && status.code() != StatusCode::NotFound) {
                    --last_applied_;
                    break;
                }
            }
        }
        applied_count++;
    }

    return applied_count;
}

} // namespace lsm
