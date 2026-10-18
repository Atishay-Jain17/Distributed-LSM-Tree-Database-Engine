#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <random>
#include "storage/sparse_index.h"

using namespace lsm;

int main() {
    std::cout << "========================================================\n"
              << "       Sparse Index vs Linear Search Microbenchmark     \n"
              << "========================================================\n";

    constexpr int kNumBlocks = 1000;
    constexpr int kLookups = 50000;

    SparseIndex index;
    std::vector<IndexEntry> raw_blocks;
    raw_blocks.reserve(kNumBlocks);

    for (int i = 0; i < kNumBlocks; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "block_key_%06d", i * 10);
        uint64_t offset = static_cast<uint64_t>(i * 4096);
        index.AddEntry(buf, offset, 4096);
        raw_blocks.push_back(IndexEntry{buf, offset, 4096});
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, (kNumBlocks * 10) - 1);

    std::vector<std::string> query_keys;
    query_keys.reserve(kLookups);
    for (int i = 0; i < kLookups; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "block_key_%06d", dist(rng));
        query_keys.emplace_back(buf);
    }

    // 1. Binary Search via SparseIndex
    IndexEntry out_entry;
    auto start = std::chrono::high_resolution_clock::now();
    for (const auto& key : query_keys) {
        index.FindCandidateBlock(key, &out_entry);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto indexed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double indexed_ops = (indexed_us > 0) ? (kLookups * 1000000.0 / indexed_us) : 0;

    // 2. Non-Indexed Linear Scan across blocks
    start = std::chrono::high_resolution_clock::now();
    for (const auto& key : query_keys) {
        for (const auto& blk : raw_blocks) {
            if (blk.key >= key) {
                out_entry = blk;
                break;
            }
        }
    }
    end = std::chrono::high_resolution_clock::now();
    auto linear_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double linear_ops = (linear_us > 0) ? (kLookups * 1000000.0 / linear_us) : 0;

    std::cout << "Blocks indexed: " << kNumBlocks << " | Queries: " << kLookups << "\n"
              << "1. Sparse Index (Binary Search): " << indexed_us << " us (" << indexed_ops << " lookups/sec)\n"
              << "2. Non-Indexed (Linear Scan)   : " << linear_us << " us (" << linear_ops << " lookups/sec)\n";

    if (linear_us > 0) {
        double speedup = static_cast<double>(linear_us) / indexed_us;
        std::cout << "Sparse Index Speedup: " << speedup << "x faster\n";
    }

    std::cout << "========================================================\n"
              << " Benchmark Completed Successfully.\n"
              << "========================================================\n";
    return 0;
}
