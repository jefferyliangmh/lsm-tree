#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "lsm/db.h"

using Clock = std::chrono::steady_clock;
static double SecsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static bool ParseInt(const char* text, int minimum, int maximum, int* value) {
    if (!text || !value) return false;
    const char* end = text;
    while (*end != '\0') ++end;
    int parsed = 0;
    const auto result = std::from_chars(text, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end ||
        parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static bool ParsePositiveInt(const char* text, int* value) {
    return ParseInt(text, 1, (std::numeric_limits<int>::max)(), value);
}

static void PrintLatency(const char* phase, std::vector<uint64_t> samples) {
    if (samples.empty()) {
        std::printf("[%s-LAT] no samples\n", phase);
        return;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double percentile) {
        return samples[static_cast<size_t>(percentile * (samples.size() - 1))];
    };
    std::printf("[%s-LAT] sampled=%zu min=%llu us p50=%llu us p95=%llu us "
        "p99=%llu us max=%llu us\n", phase, samples.size(),
        (unsigned long long)samples.front(), (unsigned long long)at(0.50),
        (unsigned long long)at(0.95), (unsigned long long)at(0.99),
        (unsigned long long)samples.back());
}

struct TempDirCleanup {
    std::filesystem::path path;
    ~TempDirCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        if (ec) std::fprintf(stderr, "benchmark cleanup failed: %s\n", ec.message().c_str());
    }
};

int main(int argc, char** argv) {
    int num_keys = 500000;
    int num_reads = 0;
    int sync_wal_arg = 0;
    if (argc > 5 || (argc > 1 && !ParsePositiveInt(argv[1], &num_keys)) ||
        (argc > 2 && !ParsePositiveInt(argv[2], &num_reads)) ||
        (argc > 3 && !ParseInt(argv[3], 0, 1, &sync_wal_arg))) {
        std::fprintf(stderr, "usage: lsm_bench [positive-key-count] [positive-read-count] [sync-wal: 0|1] [db-root]\n");
        return 2;
    }
    if (num_reads == 0) num_reads = num_keys;

    const auto run_id = Clock::now().time_since_epoch().count();
    const std::filesystem::path db_root = argc > 4
        ? std::filesystem::path(argv[4])
        : std::filesystem::temp_directory_path();
    std::error_code cleanup_error;
    std::filesystem::create_directories(db_root, cleanup_error);
    if (cleanup_error) {
        std::fprintf(stderr, "cannot create benchmark database root: %s\n",
            cleanup_error.message().c_str());
        return 2;
    }
    const std::filesystem::path path = db_root /
        ("lsm_bench_db_" + std::to_string(run_id));
    cleanup_error.clear();
    std::filesystem::remove_all(path, cleanup_error);
    if (cleanup_error) {
        std::fprintf(stderr, "cannot prepare benchmark directory: %s\n",
            cleanup_error.message().c_str());
        return 2;
    }
    TempDirCleanup cleanup{ path };

    lsm::Options opt;
    opt.db_path = path.string();
    opt.sync_wal = sync_wal_arg != 0;
    opt.level0_trigger = 4;

    // ★ 关键：500K puts / 35000 ≈ 15 次 Flush → 15+ 个 SST
    opt.memtable_entries = 35000;
    opt.memtable_bytes = 4ull * 1024 * 1024;   // 4MB

    opt.max_file_size = 16ull * 1024 * 1024;
    opt.max_bytes_for_level_base = 256ull * 1024 * 1024;
    opt.block_cache_capacity = 512ull * 1024 * 1024;
    opt.block_cache_shard_bits = 8;
    opt.block_restart_interval = 8;

    const int kNumKeys = num_keys;
    const int kReads = num_reads;
    const std::string base_value(100, 'x');
    std::vector<std::string> mixed_keys;
    std::printf("=== LSM-Tree end-to-end benchmark ===\n");
    std::printf("  CRC32C backend: %s; keys=%d; reads/phase=%d; sync_wal=%s; "
        "latency sample interval=64 ops\n  DB root: %s\n",
        lsm::CRC32C::EffectiveBackendName().c_str(), kNumKeys, kReads,
        opt.sync_wal ? "true" : "false", db_root.string().c_str());

    {
        lsm::DB db(opt);
        std::vector<uint64_t> write_latency_us;
        write_latency_us.reserve(static_cast<size_t>(kNumKeys / 64 + 1));
        auto t0 = Clock::now();
        for (int i = 0; i < kNumKeys; ++i) {
            const auto operation_start = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
            auto status = db.Put("key_" + std::to_string(i), base_value);
            if (!status.ok()) {
                std::fprintf(stderr, "Put failed at %d: %s\n", i, status.ToString().c_str());
                return 1;
            }
            if (i % 64 == 0)
                write_latency_us.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - operation_start).count()));
        }
        auto flush_status = db.Flush();
        if (!flush_status.ok()) {
            std::fprintf(stderr, "Flush failed: %s\n", flush_status.ToString().c_str());
            return 1;
        }
        double wt = SecsSince(t0);
        auto st = db.GetStats();
        std::printf("[WRITE]  %d puts in %.3fs  =>  %.0f ops/s  "
                    "(flushes=%llu, compactions=%llu, immutable_peak=%zu, "
                    "slowdowns=%llu, stops=%llu)\n",
                    kNumKeys, wt, kNumKeys / wt,
                    (unsigned long long)st.flushes,
                    (unsigned long long)st.compactions,
                    st.immutable_memtables_peak,
                    (unsigned long long)st.write_slowdowns,
                    (unsigned long long)st.write_stops);
        PrintLatency("WRITE", std::move(write_latency_us));

        std::mt19937_64 rng(42);
        std::vector<uint64_t> hit_latency_us;
        hit_latency_us.reserve(static_cast<size_t>(kReads / 64 + 1));
        t0 = Clock::now();
        int hits = 0;
        std::string v;
        for (int i = 0; i < kReads; ++i) {
            const auto operation_start = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
            const std::string key = "key_" + std::to_string(rng() % kNumKeys);
            auto s = db.Get(key, &v);
            if (!s.ok() || v != base_value) {
                std::fprintf(stderr, "hit-read verification failed for %s: %s\n",
                    key.c_str(), s.ToString().c_str());
                return 1;
            }
            ++hits;
            if (i % 64 == 0)
                hit_latency_us.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - operation_start).count()));
        }
        double rt = SecsSince(t0);
        std::printf("[READ-H] %d gets in %.3fs  =>  %.0f ops/s  (hits=%d)\n",
                    kReads, rt, kReads / rt, hits);
        PrintLatency("READ-H", std::move(hit_latency_us));

        std::vector<uint64_t> miss_latency_us;
        miss_latency_us.reserve(static_cast<size_t>(kReads / 64 + 1));
        t0 = Clock::now();
        for (int i = 0; i < kReads; ++i) {
            const auto operation_start = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
            auto status = db.Get("missing_" + std::to_string(i), &v);
            if (!status.IsNotFound()) {
                std::fprintf(stderr, "miss-read verification failed: %s\n",
                    status.ToString().c_str());
                return 1;
            }
            if (i % 64 == 0)
                miss_latency_us.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - operation_start).count()));
        }
        double mt = SecsSince(t0);
        std::printf("[READ-M] %d gets in %.3fs  =>  %.0f ops/s\n",
                    kReads, mt, kReads / mt);
        PrintLatency("READ-M", std::move(miss_latency_us));

        rng.seed(7);
        std::vector<uint64_t> mixed_latency_us;
        mixed_latency_us.reserve(static_cast<size_t>(kReads / 64 + 1));
        int mixed_puts = 0, mixed_gets = 0;
        t0 = Clock::now();
        for (int i = 0; i < kReads; ++i) {
            const auto operation_start = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
            if (rng() & 1) {
                std::string key = "mixed_" + std::to_string(i);
                auto status = db.Put(key, "mixed-value");
                if (!status.ok()) {
                    std::fprintf(stderr, "mixed Put failed: %s\n", status.ToString().c_str());
                    return 1;
                }
                mixed_keys.push_back(std::move(key));
                ++mixed_puts;
            } else {
                const std::string key = "key_" + std::to_string(rng() % kNumKeys);
                auto status = db.Get(key, &v);
                if (!status.ok() || v != base_value) {
                    std::fprintf(stderr, "mixed Get verification failed: %s\n",
                        status.ToString().c_str());
                    return 1;
                }
                ++mixed_gets;
            }
            if (i % 64 == 0)
                mixed_latency_us.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - operation_start).count()));
        }
        double xt = SecsSince(t0);
        std::printf("[MIXED] %d ops in %.3fs  =>  %.0f ops/s (puts=%d gets=%d)\n",
                    kReads, xt, kReads / xt, mixed_puts, mixed_gets);
        PrintLatency("MIXED", std::move(mixed_latency_us));
    }

    {
        auto t0 = Clock::now();
        lsm::DB db(opt);
        double open_time = SecsSince(t0);
        const auto verify_start = Clock::now();
        std::string v;
        for (int i = 0; i < kNumKeys; ++i) {
            auto status = db.Get("key_" + std::to_string(i), &v);
            if (!status.ok() || v != base_value) {
                std::fprintf(stderr, "recovery verification failed for key_%d: %s\n",
                    i, status.ToString().c_str());
                return 1;
            }
        }
        for (const auto& key : mixed_keys) {
            auto status = db.Get(key, &v);
            if (!status.ok() || v != "mixed-value") {
                std::fprintf(stderr, "recovery verification failed for %s: %s\n",
                    key.c_str(), status.ToString().c_str());
                return 1;
            }
        }
        std::printf("[RECOV] open=%.4fs verify=%.3fs; verified %d base + %zu mixed keys\n",
            open_time, SecsSince(verify_start), kNumKeys, mixed_keys.size());
    }

    return 0;
}
