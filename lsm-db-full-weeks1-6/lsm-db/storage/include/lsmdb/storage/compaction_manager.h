#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "lsmdb/storage/compaction_job.h"
#include "lsmdb/storage/compaction_picker.h"

namespace lsmdb::storage {

// Background worker: one thread, one compaction at a time.
// Call Notify() after adding a file to L0 (a flush). The worker keeps compacting until
// the picker finds nothing left to do, then sleeps.
class CompactionManager {
 public:
  using SnapshotFn = std::function<std::uint64_t()>;  // oldest live snapshot seq

  CompactionManager(VersionSet& versions, CompactionOptions options, SnapshotFn oldest_snapshot = nullptr);
  ~CompactionManager();

  void Start();
  void Stop();  // idempotent; waits for a running compaction to finish
  void Notify();

  // Synchronous: performs at most one compaction. Returns nullopt if nothing to do.
  // Safe to call while the worker runs (they take turns).
  std::optional<CompactionStats> RunOnce();

  // True once no compaction is pending or running (false on timeout).
  bool WaitUntilIdle(std::chrono::milliseconds timeout);

  std::uint64_t compactions_completed() const;
  std::string last_error() const;

 private:
  void WorkerLoop();

  VersionSet& versions_;
  CompactionOptions options_;
  SnapshotFn oldest_snapshot_;
  CompactionPicker picker_;

  std::mutex run_mu_;  // serializes RunOnce / picker state
  mutable std::mutex mu_;
  std::condition_variable cv_, idle_cv_;
  bool stop_ = false, wake_ = false, busy_ = false;
  std::uint64_t completed_ = 0;
  std::string last_error_;
  std::thread worker_;
};

}  // namespace lsmdb::storage
