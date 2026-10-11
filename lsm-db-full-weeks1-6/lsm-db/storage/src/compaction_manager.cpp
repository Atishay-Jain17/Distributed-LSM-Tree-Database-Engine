#include "lsmdb/storage/compaction_manager.h"

namespace lsmdb::storage {

CompactionManager::CompactionManager(VersionSet& versions, CompactionOptions options, SnapshotFn oldest_snapshot)
    : versions_(versions),
      options_(options),
      oldest_snapshot_(oldest_snapshot ? std::move(oldest_snapshot) : SnapshotFn([] { return kMaxSequence; })),
      picker_(options) {}

CompactionManager::~CompactionManager() { Stop(); }

void CompactionManager::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (worker_.joinable()) return;
  stop_ = false;
  wake_ = true;  // look for backlog immediately
  worker_ = std::thread([this] { WorkerLoop(); });
}

void CompactionManager::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void CompactionManager::Notify() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    wake_ = true;
  }
  cv_.notify_one();
}

std::optional<CompactionStats> CompactionManager::RunOnce() {
  std::lock_guard<std::mutex> run_lock(run_mu_);
  auto task = picker_.Pick(versions_.Current());
  if (!task) return std::nullopt;
  CompactionStats stats = RunCompaction(*task, versions_, options_, oldest_snapshot_());
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++completed_;
  }
  return stats;
}

void CompactionManager::WorkerLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [&] { return stop_ || wake_; });
    if (stop_) break;
    wake_ = false;
    busy_ = true;
    lock.unlock();
    try {
      while (!stop_ && RunOnce()) {
      }
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> err_lock(mu_);
      last_error_ = e.what();
    }
    lock.lock();
    busy_ = false;
    idle_cv_.notify_all();
  }
}

bool CompactionManager::WaitUntilIdle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  return idle_cv_.wait_for(lock, timeout, [&] { return !wake_ && !busy_; });
}

std::uint64_t CompactionManager::compactions_completed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return completed_;
}

std::string CompactionManager::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

}  // namespace lsmdb::storage
