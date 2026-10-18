#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <random>
#include "storage/memtable.h"

using namespace lsm;

int main() {
    std::cout << "========================================================\n"
              << "       LSM MemTable (SkipList) Microbenchmark           \n"
              << "========================================================\n";

    constexpr int kNumOps = 20000;
    MemTable memtable;

    // 1. Benchmark Sequential Insert
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kNumOps; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "bench_key_%08d", i);
        memtable.Put(buf, "bench_value_payload_1234567890");
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto insert_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double insert_ops = (insert_ms > 0) ? (kNumOps * 1000.0 / insert_ms) : 0;
    std::cout << "1. Sequential Insert (" << kNumOps << " ops): "
              << insert_ms << " ms | " << insert_ops << " ops/sec\n";

    // 2. Benchmark Random Lookup
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, kNumOps - 1);
    std::string val;
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kNumOps; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "bench_key_%08d", dist(rng));
        memtable.Get(buf, &val);
    }
    end = std::chrono::high_resolution_clock::now();
    auto lookup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double lookup_ops = (lookup_ms > 0) ? (kNumOps * 1000.0 / lookup_ms) : 0;
    std::cout << "2. Random Lookup     (" << kNumOps << " ops): "
              << lookup_ms << " ms | " << lookup_ops << " ops/sec\n";

    // 3. Benchmark Ordered Scan
    start = std::chrono::high_resolution_clock::now();
    size_t scanned = 0;
    for (auto it = memtable.NewIterator(); it.Valid(); it.Next()) {
        scanned++;
    }
    end = std::chrono::high_resolution_clock::now();
    auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double scan_ops = (scan_ms > 0) ? (scanned * 1000.0 / scan_ms) : 0;
    std::cout << "3. In-Order Scan     (" << scanned << " items): "
              << scan_ms << " ms | " << scan_ops << " items/sec\n";

    // 4. Memory Footprint
    double mb = static_cast<double>(memtable.ApproximateMemoryUsage()) / (1024.0 * 1024.0);
    std::cout << "4. Memory Accounting : "
              << memtable.ApproximateMemoryUsage() << " bytes (~" << mb << " MB)\n";

    std::cout << "========================================================\n"
              << " Benchmark Completed Successfully.\n"
              << "========================================================\n";
    return 0;
}
