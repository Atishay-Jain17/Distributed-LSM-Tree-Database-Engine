// Measures MemTable throughput under contention: shared (reader/writer) lock versus an
// exclusive-only lock, for read-heavy and mixed workloads.
//   ./build/benchmarks/memtable_contention
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "lsmdb/storage/memtable.h"

using namespace lsmdb::storage;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int kKeys = 20000;
constexpr auto kDuration = std::chrono::milliseconds(400);

double RunOnce(MemTable::LockMode mode, int threads, int read_percent) {
  MemTable table(mode);
  for (int i = 0; i < kKeys; ++i) (void)table.Put("key" + std::to_string(i), "value", 1 + i);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> total{0};
  std::atomic<std::uint64_t> seq{kKeys + 1};
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937 rng(1234 + t);
      std::uniform_int_distribution<int> key_dist(0, kKeys - 1), pct(0, 99);
      std::uint64_t ops = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        std::string key = "key" + std::to_string(key_dist(rng));
        if (pct(rng) < read_percent) (void)table.Get(key);
        else (void)table.Put(std::move(key), "value", seq.fetch_add(1));
        ++ops;
      }
      total += ops;
    });
  }
  auto start = Clock::now();
  std::this_thread::sleep_for(kDuration);
  stop = true;
  for (auto& w : workers) w.join();
  double secs = std::chrono::duration<double>(Clock::now() - start).count();
  return static_cast<double>(total.load()) / secs;
}
}  // namespace

int main() {
  std::printf("hardware threads: %u\n\n", std::thread::hardware_concurrency());
  std::printf("%-8s %-8s %16s %16s %8s\n", "threads", "read%", "shared ops/s", "exclusive ops/s", "speedup");
  for (int read_percent : {95, 50}) {
    for (int threads : {1, 2, 4, 8}) {
      double shared = RunOnce(MemTable::LockMode::kSharedReaders, threads, read_percent);
      double excl = RunOnce(MemTable::LockMode::kExclusiveOnly, threads, read_percent);
      std::printf("%-8d %-8d %16.0f %16.0f %7.2fx\n", threads, read_percent, shared, excl, shared / excl);
    }
  }
  return 0;
}
