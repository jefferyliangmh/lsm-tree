#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "lsm/memtable.h"

static double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

static void Run(lsm::MemTableType type, int n, const std::vector<int>& keys) {
    lsm::MemTable table(type);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        table.Add(static_cast<uint64_t>(i + 1), std::to_string(keys[i]),
            "value-" + std::to_string(i), lsm::ValueType::kValue);
    }
    const double insert_ms = MsSince(t0);
    int hits = 0;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        auto r = table.Get(std::to_string(keys[i]), static_cast<uint64_t>(n + 1));
        hits += r.found ? 1 : 0;
    }
    const double get_ms = MsSince(t0);
    std::printf("%-18s n=%d insert=%.1f ms (%.0f ops/s) get=%.1f ms (%.0f ops/s) hits=%d mem=%zu\n",
        lsm::MemTableTypeName(type), n, insert_ms, n * 1000.0 / insert_ms,
        get_ms, n * 1000.0 / get_ms, hits, table.MemoryUsage());
}

int main(int argc, char** argv) {
    int n = 20000;
    if (argc > 1) n = std::atoi(argv[1]);
    if (n < 100) n = 100;
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, n * 2);
    std::vector<int> keys(n);
    for (int i = 0; i < n; ++i) keys[i] = dist(rng);
    std::printf("memtable point-lookup comparison\n");
    Run(lsm::MemTableType::kSkipList, n, keys);
    Run(lsm::MemTableType::kAppendOnlySimd, n, keys);
    return 0;
}
