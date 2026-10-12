#include "storage/memtable.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kOperations = 20000;
constexpr int kRuns = 3;

std::string Key(std::size_t i) {
    const std::string value = std::to_string(i);
    return "key-" + std::string(8 - value.size(), '0') + value;
}

struct Result { double throughput_ops_sec; std::uint64_t p99_us; };

template <typename Fn>
Result Measure(Fn&& operation) {
    std::vector<std::uint64_t> latencies;
    latencies.reserve(kOperations);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < kOperations; ++i) {
        const auto before = Clock::now();
        operation(i);
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(after - before).count()));
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(latencies.begin(), latencies.end());
    const auto p99_index = (latencies.size() * 99 + 99) / 100 - 1;
    return {static_cast<double>(kOperations) / elapsed, latencies[p99_index]};
}
}

int main() {
    std::cout << "Atishay storage benchmark; operations=" << kOperations
              << "; runs=" << kRuns << "; deterministic sequential workload\n"
              << "run,write_ops_sec,write_p99_us,read_ops_sec,read_p99_us\n";
    for (int run = 1; run <= kRuns; ++run) {
        lsm::MemTable table;
        const Result writes = Measure([&](std::size_t i) { table.Put(Key(i), "value-" + std::to_string(i)); });
        const Result reads = Measure([&](std::size_t i) {
            std::string value;
            table.Get(Key((i * 7919) % kOperations), &value);
        });
        std::cout << run << ',' << std::fixed << std::setprecision(2) << writes.throughput_ops_sec
                  << ',' << writes.p99_us << ',' << reads.throughput_ops_sec << ',' << reads.p99_us << '\n';
    }
    return 0;
}
