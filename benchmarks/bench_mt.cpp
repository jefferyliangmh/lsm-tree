#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "lsm/db.h"

using Clock = std::chrono::steady_clock;
static double SecsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static bool ParseInteger(const char* text, int minimum, int maximum, int* value) {
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

static void PrintLatency(const char* label, std::vector<uint64_t> samples) {
    if (samples.empty()) {
        std::printf("[%s-LAT] no samples\n", label);
        return;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double percentile) {
        return samples[static_cast<size_t>(percentile * (samples.size() - 1))];
    };
    std::printf("[%s-LAT] sampled=%zu min=%llu us p50=%llu us p95=%llu us "
        "p99=%llu us max=%llu us\n", label, samples.size(),
        (unsigned long long)samples.front(), (unsigned long long)at(0.50),
        (unsigned long long)at(0.95), (unsigned long long)at(0.99),
        (unsigned long long)samples.back());
}

static uint64_t Delta(uint64_t before, uint64_t after) {
    return after >= before ? after - before : 0;
}

static void PrintWritePathDelta(const lsm::WritePathStats& before,
    const lsm::WritePathStats& after, uint64_t operations) {
    const uint64_t capacity = Delta(before.capacity_wait_ns, after.capacity_wait_ns);
    const uint64_t lock = Delta(before.write_lock_wait_ns, after.write_lock_wait_ns);
    const uint64_t wal = Delta(before.wal_append_ns, after.wal_append_ns);
    const uint64_t mem_lock = Delta(before.memtable_lock_wait_ns, after.memtable_lock_wait_ns);
    const uint64_t mem_add = Delta(before.memtable_add_ns, after.memtable_add_ns);
    const uint64_t rotation = Delta(before.rotation_ns, after.rotation_ns);
    const uint64_t measured = capacity + lock + wal + mem_lock + mem_add + rotation;
    const auto ns_per_op = [operations](uint64_t value) {
        return operations == 0 ? 0.0 : static_cast<double>(value) / operations;
    };
    const auto share = [measured](uint64_t value) {
        return measured == 0 ? 0.0 : 100.0 * static_cast<double>(value) / measured;
    };
    const uint64_t batches = Delta(before.wal_batches, after.wal_batches);
    const uint64_t records = Delta(before.wal_records, after.wal_records);
    const uint64_t flushes = Delta(before.background_flushes, after.background_flushes);
    const uint64_t flush_ns = Delta(before.background_flush_ns, after.background_flush_ns);
    std::printf("[WRITE-PATH] measured-stage sum=%.3f ms; avg ns/op: "
        "capacity=%.0f (%.1f%%), write_lock=%.0f (%.1f%%), WAL=%.0f (%.1f%%), "
        "memtable_lock=%.0f (%.1f%%), memtable_add=%.0f (%.1f%%), "
        "rotation=%.0f (%.1f%%)\n",
        measured / 1e6, ns_per_op(capacity), share(capacity), ns_per_op(lock), share(lock),
        ns_per_op(wal), share(wal), ns_per_op(mem_lock), share(mem_lock),
        ns_per_op(mem_add), share(mem_add),
        ns_per_op(rotation), share(rotation));
    std::printf("[WAL-DETAIL] avg ns/op: mutex=%.0f queue=%.0f append_io=%.0f sync=%.0f; "
        "batches=%llu records=%llu avg_batch=%.2f\n",
        ns_per_op(Delta(before.wal_mutex_wait_ns, after.wal_mutex_wait_ns)),
        ns_per_op(Delta(before.wal_queue_wait_ns, after.wal_queue_wait_ns)),
        ns_per_op(Delta(before.wal_append_io_ns, after.wal_append_io_ns)),
        ns_per_op(Delta(before.wal_sync_ns, after.wal_sync_ns)),
        (unsigned long long)batches, (unsigned long long)records,
        batches == 0 ? 0.0 : static_cast<double>(records) / batches);
    std::printf("[BG-FLUSH] count=%llu aggregate=%.3f ms avg=%.3f ms\n",
        (unsigned long long)flushes, flush_ns / 1e6,
        flushes == 0 ? 0.0 : flush_ns / 1e6 / flushes);
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
    int num_threads = 8;
    int num_keys = 500000;
    int ops_per_thread = 200000;
    int sync_wal_arg = 0;
    int diagnostics_arg = 0;
    if (argc > 7 || (argc > 1 && !ParseInteger(argv[1], 1, 256, &num_threads)) ||
        (argc > 2 && !ParseInteger(argv[2], 1, (std::numeric_limits<int>::max)(), &num_keys)) ||
        (argc > 3 && !ParseInteger(argv[3], 1, (std::numeric_limits<int>::max)(), &ops_per_thread)) ||
        (argc > 4 && !ParseInteger(argv[4], 0, 1, &sync_wal_arg)) ||
        (argc > 6 && !ParseInteger(argv[6], 0, 1, &diagnostics_arg)) ||
        static_cast<uint64_t>(num_threads) * static_cast<uint64_t>(ops_per_thread) >
            (std::numeric_limits<size_t>::max)()) {
        std::fprintf(stderr, "usage: lsm_bench_mt [threads:1..256] [key-count] [ops-per-thread] [sync-wal: 0|1] [db-root] [diagnostics: 0|1]\n");
        return 2;
    }
    const auto run_id = Clock::now().time_since_epoch().count();
    const std::filesystem::path db_root = argc > 5
        ? std::filesystem::path(argv[5])
        : std::filesystem::temp_directory_path();
    std::error_code cleanup_error;
    std::filesystem::create_directories(db_root, cleanup_error);
    if (cleanup_error) {
        std::fprintf(stderr, "cannot create benchmark database root: %s\n",
            cleanup_error.message().c_str());
        return 2;
    }
    const std::filesystem::path path = db_root /
        ("lsm_bench_mt_db_" + std::to_string(run_id));
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
    opt.memtable_entries = 35000;
    opt.memtable_bytes = 4ull * 1024 * 1024;
    opt.max_file_size = 16ull * 1024 * 1024;
    opt.max_bytes_for_level_base = 256ull * 1024 * 1024;
    opt.block_cache_capacity = 512ull * 1024 * 1024;
    opt.block_cache_shard_bits = 8;
    opt.block_restart_interval = 8;
    opt.collect_write_path_stats = diagnostics_arg != 0;

    std::printf("=== Multi-thread benchmark (%d threads; keys=%d; ops/thread=%d; "
        "sync_wal=%s; diagnostics=%s; latency sample interval=64 ops) ===\n  DB root: %s\n",
        num_threads, num_keys, ops_per_thread, opt.sync_wal ? "true" : "false",
        opt.collect_write_path_stats ? "true" : "false", db_root.string().c_str());

    const int kKeys = num_keys;
    const int kOpsPerThread = ops_per_thread;
    const uint64_t mixed_puts_expected = static_cast<uint64_t>(num_threads) *
        (static_cast<uint64_t>(kOpsPerThread) / 2 +
            static_cast<uint64_t>(kOpsPerThread) % 2);
    std::string val(100, 'x');

    {
        lsm::DB db(opt);

        // ====================================================================
        // 阶段 1：写入 + 诊断
        // ====================================================================
        std::printf("--- Write phase ---\n");
        auto t_write = Clock::now();

        int put_errors = 0;
        for (int i = 0; i < kKeys; ++i) {
            auto st = db.Put("key_" + std::to_string(i), val);
            if (!st.ok()) {
                if (put_errors < 10)
                    std::fprintf(stderr, "[Put FAIL] key_%d: %s\n",
                        i, st.ToString().c_str());
                ++put_errors;
            }
        }
        double write_secs = SecsSince(t_write);
        std::printf("  Put: %.3fs, %.0f ops/s, errors=%d\n",
            write_secs, kKeys / write_secs, put_errors);
        if (put_errors != 0) return 1;

        std::printf("--- Concurrent write/backpressure phase (%d threads) ---\n",
            num_threads);
        std::vector<std::vector<uint64_t>> latency_by_thread(num_threads);
        std::atomic<uint64_t> concurrent_write_errors{ 0 };
        std::vector<std::thread> writers;
        writers.reserve(num_threads);
        const auto write_path_before = db.GetStats().write_path;
        auto concurrent_write_start = Clock::now();
        for (int thread = 0; thread < num_threads; ++thread) {
            writers.emplace_back([&, thread] {
                auto& samples = latency_by_thread[thread];
                samples.reserve(static_cast<size_t>(ops_per_thread / 64 + 1));
                for (int i = 0; i < ops_per_thread; ++i) {
                    const auto operation_start = (i % 64 == 0)
                        ? Clock::now() : Clock::time_point{};
                    auto status = db.Put("mt_" + std::to_string(thread) + "_" +
                        std::to_string(i), std::to_string(thread) + ":" +
                        std::to_string(i));
                    if (!status.ok()) {
                        concurrent_write_errors.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (i % 64 == 0)
                        samples.push_back(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                Clock::now() - operation_start).count()));
                }
            });
        }
        for (auto& writer : writers) writer.join();
        const double concurrent_write_secs = SecsSince(concurrent_write_start);
        if (concurrent_write_errors.load(std::memory_order_relaxed) != 0) {
            std::fprintf(stderr, "concurrent write failures: %llu\n",
                (unsigned long long)concurrent_write_errors.load());
            return 1;
        }
        std::vector<uint64_t> write_latency_us;
        for (auto& samples : latency_by_thread)
            write_latency_us.insert(write_latency_us.end(), samples.begin(), samples.end());
        const uint64_t concurrent_ops = static_cast<uint64_t>(num_threads) *
            static_cast<uint64_t>(ops_per_thread);
        std::printf("  Writes: %llu ops in %.3fs => %.0f ops/s\n",
            (unsigned long long)concurrent_ops, concurrent_write_secs,
            concurrent_ops / concurrent_write_secs);
        PrintLatency("MT-WRITE", std::move(write_latency_us));
        if (opt.collect_write_path_stats)
            PrintWritePathDelta(write_path_before, db.GetStats().write_path, concurrent_ops);

        auto t_flush = Clock::now();
        auto st_flush = db.Flush();
        double flush_secs = SecsSince(t_flush);
        std::printf("  Flush: %.3fs, status=%s\n",
            flush_secs, st_flush.ok() ? "OK" : st_flush.ToString().c_str());
        if (!st_flush.ok()) return 1;

        auto stats = db.GetStats();
        std::printf("  Stats: puts=%llu flushes=%llu compactions=%llu immutable=%zu "
            "immutable_peak=%zu slowdowns=%llu stops=%llu levels=",
            (unsigned long long)stats.puts,
            (unsigned long long)stats.flushes,
            (unsigned long long)stats.compactions,
            stats.immutable_memtables,
            stats.immutable_memtables_peak,
            (unsigned long long)stats.write_slowdowns,
            (unsigned long long)stats.write_stops);
        for (auto n : stats.level_files) std::printf(" %zu", n);
        std::printf("\n");

        // ====================================================================
        // 阶段 2：单线程全量验证（关键：定位 2% 丢失）
        // ====================================================================
        std::printf("--- Single-thread verify phase ---\n");
        auto t_verify = Clock::now();
        int get_missing = 0;
        int get_wrong_value = 0;
        std::string vv;
        for (int i = 0; i < kKeys; ++i) {
            std::string k = "key_" + std::to_string(i);
            auto s = db.Get(k, &vv);
            if (!s.ok()) {
                if (get_missing < 10)
                    std::fprintf(stderr, "[Verify MISSING] %s\n", k.c_str());
                ++get_missing;
            }
            else if (vv != val) {
                if (get_wrong_value < 10)
                    std::fprintf(stderr, "[Verify WRONG] %s value_size=%zu\n",
                        k.c_str(), vv.size());
                ++get_wrong_value;
            }
        }
        double verify_secs = SecsSince(t_verify);
        std::printf("  Verify: %.3fs, %d ops/s, missing=%d wrong=%d\n",
            verify_secs,
            static_cast<int>(kKeys / verify_secs),
            get_missing, get_wrong_value);

        // 如果单线程验证就失败，多线程测试没有意义
        if (get_missing > 0 || get_wrong_value > 0) {
            std::printf("\n[ABORT] data integrity broken, skip MT benchmark\n");
            return 1;
        }

        // ====================================================================
        // 阶段 3：多线程并发读
        // ====================================================================
        std::printf("--- Multi-thread read phase (%d threads) ---\n", num_threads);
        auto t0 = Clock::now();
        std::atomic<uint64_t> total_hits{ 0 };
        std::atomic<uint64_t> total_miss{ 0 };
        std::atomic<uint64_t> total_wrong{ 0 };
        std::vector<std::vector<uint64_t>> read_latency_by_thread(num_threads);
        std::vector<std::thread> ts;
        ts.reserve(num_threads);

        for (int t = 0; t < num_threads; ++t) {
            ts.emplace_back([&, t] {
                auto& latency = read_latency_by_thread[t];
                latency.reserve(static_cast<size_t>(kOpsPerThread / 64 + 1));
                std::mt19937_64 rng(t * 123456789ULL + 42);
                std::string v;
                uint64_t hits = 0, miss = 0, wrong = 0;
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const auto operation_start = (i % 64 == 0)
                        ? Clock::now() : Clock::time_point{};
                    std::string key = "key_" + std::to_string(rng() % kKeys);
                    auto status = db.Get(key, &v);
                    if (!status.ok()) ++miss;
                    else if (v != val) ++wrong;
                    else ++hits;
                    if (i % 64 == 0)
                        latency.push_back(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                Clock::now() - operation_start).count()));
                }
                total_hits.fetch_add(hits, std::memory_order_relaxed);
                total_miss.fetch_add(miss, std::memory_order_relaxed);
                total_wrong.fetch_add(wrong, std::memory_order_relaxed);
                });
        }
        for (auto& th : ts) th.join();

        double dt = SecsSince(t0);
        double total_ops = static_cast<double>(num_threads) * kOpsPerThread;
        uint64_t hits = total_hits.load();
        uint64_t miss = total_miss.load();
        uint64_t wrong = total_wrong.load();
        std::printf("[MT-READ] %d threads × %d ops in %.3fs  =>  %.0f ops/s  "
            "(hits=%llu miss=%llu wrong=%llu hit_rate=%.2f%%)\n",
            num_threads, kOpsPerThread, dt, total_ops / dt,
            (unsigned long long)hits,
            (unsigned long long)miss,
            (unsigned long long)wrong,
            total_ops > 0 ? 100.0 * hits / total_ops : 0.0);
        std::vector<uint64_t> read_latency_us;
        for (auto& samples : read_latency_by_thread)
            read_latency_us.insert(read_latency_us.end(), samples.begin(), samples.end());
        PrintLatency("MT-READ", std::move(read_latency_us));
        if (miss != 0 || wrong != 0) return 1;

        std::printf("--- Concurrent mixed read/write phase (%d threads; 50%% puts) ---\n",
            num_threads);
        std::vector<std::vector<uint64_t>> mixed_latency_by_thread(num_threads);
        std::atomic<uint64_t> mixed_errors{ 0 };
        ts.clear();
        ts.reserve(num_threads);
        const auto mixed_write_path_before = db.GetStats().write_path;
        auto mixed_start = Clock::now();
        for (int t = 0; t < num_threads; ++t) {
            ts.emplace_back([&, t] {
                auto& latency = mixed_latency_by_thread[t];
                latency.reserve(static_cast<size_t>(kOpsPerThread / 64 + 1));
                std::mt19937_64 rng(static_cast<uint64_t>(t) * 987654321ULL + 17);
                std::string value;
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const auto operation_start = (i % 64 == 0)
                        ? Clock::now() : Clock::time_point{};
                    if ((i % 2) == 0) {
                        const std::string key = "mix_" + std::to_string(t) + "_" +
                            std::to_string(i);
                        const std::string mixed_value = "mix:" + std::to_string(t) + ":" +
                            std::to_string(i);
                        if (!db.Put(key, mixed_value).ok())
                            mixed_errors.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        const std::string key = "key_" +
                            std::to_string(rng() % static_cast<uint64_t>(kKeys));
                        const auto status = db.Get(key, &value);
                        if (!status.ok() || value != val)
                            mixed_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (i % 64 == 0)
                        latency.push_back(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                Clock::now() - operation_start).count()));
                }
            });
        }
        for (auto& th : ts) th.join();
        const double mixed_secs = SecsSince(mixed_start);
        const uint64_t mixed_total_ops = static_cast<uint64_t>(num_threads) *
            static_cast<uint64_t>(kOpsPerThread);
        std::printf("[MT-MIXED] %llu ops in %.3fs => %.0f ops/s (puts=%llu gets=%llu errors=%llu)\n",
            (unsigned long long)mixed_total_ops, mixed_secs, mixed_total_ops / mixed_secs,
            (unsigned long long)mixed_puts_expected,
            (unsigned long long)(mixed_total_ops - mixed_puts_expected),
            (unsigned long long)mixed_errors.load(std::memory_order_relaxed));
        std::vector<uint64_t> mixed_latency_us;
        for (auto& samples : mixed_latency_by_thread)
            mixed_latency_us.insert(mixed_latency_us.end(), samples.begin(), samples.end());
        PrintLatency("MT-MIXED", std::move(mixed_latency_us));
        if (mixed_errors.load(std::memory_order_relaxed) != 0) return 1;
        if (opt.collect_write_path_stats)
            PrintWritePathDelta(mixed_write_path_before, db.GetStats().write_path,
                mixed_puts_expected);
        const auto mixed_flush_start = Clock::now();
        const auto mixed_flush_status = db.Flush();
        std::printf("[MT-MIXED-FLUSH] %.3fs status=%s\n", SecsSince(mixed_flush_start),
            mixed_flush_status.ok() ? "OK" : mixed_flush_status.ToString().c_str());
        if (!mixed_flush_status.ok()) return 1;

        // ====================================================================
        // 阶段 4：再次单线程验证（检查多线程读是否影响数据）
        // ====================================================================
        std::printf("--- Post-MT verify phase ---\n");
        int post_errors = 0;
        for (int i = 0; i < kKeys; ++i) {
            std::string k = "key_" + std::to_string(i);
            auto status = db.Get(k, &vv);
            if (!status.ok() || vv != val) ++post_errors;
        }
        for (int thread = 0; thread < num_threads; ++thread) {
            for (int i = 0; i < kOpsPerThread; ++i) {
                if ((i % 2) != 0) continue;
                const std::string expected = "mix:" + std::to_string(thread) + ":" +
                    std::to_string(i);
                auto status = db.Get("mix_" + std::to_string(thread) + "_" +
                    std::to_string(i), &vv);
                if (!status.ok() || vv != expected) ++post_errors;
            }
        }
        std::printf("  Post-MT verify: errors=%d\n", post_errors);
        if (post_errors != 0) return 1;
    }

    std::printf("--- Recovery verification ---\n");
    auto recovery_start = Clock::now();
    {
        lsm::DB db(opt);
        std::string value;
        for (int i = 0; i < kKeys; ++i) {
            auto status = db.Get("key_" + std::to_string(i), &value);
            if (!status.ok() || value != val) {
                std::fprintf(stderr, "recovery mismatch for base key %d: %s\n",
                    i, status.ToString().c_str());
                return 1;
            }
        }
        for (int thread = 0; thread < num_threads; ++thread) {
            for (int i = 0; i < kOpsPerThread; ++i) {
                const std::string expected = std::to_string(thread) + ":" +
                    std::to_string(i);
                auto status = db.Get("mt_" + std::to_string(thread) + "_" +
                    std::to_string(i), &value);
                if (!status.ok() || value != expected) {
                    std::fprintf(stderr, "recovery mismatch for mt key %d:%d: %s\n",
                        thread, i, status.ToString().c_str());
                    return 1;
                }
            }
            for (int i = 0; i < kOpsPerThread; ++i) {
                if ((i % 2) != 0) continue;
                const std::string expected = "mix:" + std::to_string(thread) + ":" +
                    std::to_string(i);
                auto status = db.Get("mix_" + std::to_string(thread) + "_" +
                    std::to_string(i), &value);
                if (!status.ok() || value != expected) {
                    std::fprintf(stderr, "recovery mismatch for mixed key %d:%d: %s\n",
                        thread, i, status.ToString().c_str());
                    return 1;
                }
            }
        }
    }
    std::printf("  Verified %llu keys after restart in %.3fs\n",
        (unsigned long long)(static_cast<uint64_t>(kKeys) +
            static_cast<uint64_t>(num_threads) * kOpsPerThread + mixed_puts_expected),
        SecsSince(recovery_start));
    return 0;
}
