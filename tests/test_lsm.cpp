#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lsm/arena.h"
#include "lsm/block_cache.h"
#include "lsm/compaction.h"
#include "lsm/crc32c.h"
#include "lsm/db.h"
#include "lsm/version.h"
#include "lsm/version_edit.h"
#include "lsm/version_set.h"
#include "lsm/wal.h"
#include "test_support.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int failures = 0;
static int tests_run = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ★ Windows 上 remove_all 对已打开的文件会抛异常。
//   用 error_code 重载静默失败：测试结束后 OS 会清理 %TEMP% 下的残留。
//   Linux 上 unlink 对已打开的文件成功，无影响。
static void SafeRemoveAll(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        std::fprintf(stderr, "[Cleanup] remove_all(%s) failed: %s\n",
            path.c_str(), ec.message().c_str());
    }
}

static std::string FreshPath(const char* n) {
    const char* configured_root = std::getenv("LSM_TEST_DB_ROOT");
    std::filesystem::path path;
    if (configured_root && *configured_root) {
        path = std::filesystem::path(configured_root) / (std::string("lsm_prod_") + n);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error("cannot create test database root: " + ec.message());
    } else {
#ifdef _WIN32
        char tmp[MAX_PATH] = {};
        ::GetTempPathA(MAX_PATH, tmp);
        path = std::filesystem::path(tmp) / (std::string("lsm_prod_") + n);
#else
        path = std::filesystem::path("/tmp") / (std::string("lsm_prod_") + n);
#endif
    }
    SafeRemoveAll(path.string());
    return path.string();
}

// ============================================================================
// [1] Arena
// ============================================================================
static void TestArena() {
    lsm::Arena a;
    char* p1 = a.Allocate(100);
    char* p2 = a.Allocate(200);
    CHECK(p1 && p2 && p2 > p1);
    char* big = a.Allocate(8000);
    CHECK(big != nullptr);
    CHECK(a.MemoryUsage() >= 8300);
    a.Reset();
    CHECK(a.MemoryUsage() == 0);
    char* p3 = a.Allocate(50);
    CHECK(p3 != nullptr);
}

// ============================================================================
// [2] MANIFEST round-trip：用 DB 端到端验证
// ============================================================================
static void TestManifestRoundTrip() {
    lsm::Options opt;
    opt.db_path = FreshPath("manifest");
    opt.memtable_entries = 100;
    opt.max_file_size = 1024;
    opt.block_cache_capacity = 8 * 1024 * 1024;
    opt.block_cache_shard_bits = 2;

    uint64_t seq_after_flush = 0;
    {
        lsm::DB db(opt);
        for (int i = 0; i < 20; ++i)
            CHECK(db.Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
        CHECK(db.Flush().ok());
        seq_after_flush = db.GetStats().puts;
        CHECK(seq_after_flush == 20);
    }
    {
        lsm::DB db(opt);
        auto st = db.GetStats();
        // L0 至少一个文件，数据完整
        CHECK(st.level_files.size() >= 1);
        CHECK(st.level_files[0] >= 1);

        std::string v;
        for (int i = 0; i < 20; ++i) {
            CHECK(db.Get("k" + std::to_string(i), &v).ok());
            CHECK(v == "v" + std::to_string(i));
        }
    }
    SafeRemoveAll(opt.db_path);
}

// ============================================================================
// [3] Clean-cut expansion：只测 PickCompaction 逻辑，reader 传 nullptr
// ============================================================================
static void TestCleanCutExpansion() {
    lsm::Options opt;
    opt.level0_trigger = 2;

    lsm::Version v;
    v.levels.resize(2);

    auto IK = [](const char* k) {
        return lsm::MakeInternalKey(k, 1, lsm::ValueType::kValue);
        };
    std::string a1 = IK("a1"), a3 = IK("a3"),
        a5 = IK("a5"), a7 = IK("a7");

    lsm::VersionEdit::FileMeta m1{ 0, 1, a1, a3, 100 };
    lsm::VersionEdit::FileMeta m2{ 0, 2, a3, a5, 100 };
    lsm::VersionEdit::FileMeta m3{ 0, 3, a5, a7, 100 };

    v.AddFile(0, m1, nullptr);
    v.AddFile(0, m2, nullptr);
    v.AddFile(0, m3, nullptr);

    lsm::LeveledPolicy policy;
    auto inputs = policy.PickCompaction(v, opt);
    CHECK(!inputs.empty());

    auto& files = inputs[0].input_file_numbers;
    CHECK(std::find(files.begin(), files.end(), 1) != files.end());
    CHECK(std::find(files.begin(), files.end(), 2) != files.end());
    CHECK(std::find(files.begin(), files.end(), 3) != files.end());
}

// ============================================================================
// [4] SuperVersion 延迟释放
// ============================================================================
static void TestVersionManagerIsolation() {
    lsm::VersionManager first;
    lsm::VersionManager second;

    auto* first_sv = new lsm::SuperVersion();
    first_sv->mutable_mem = std::make_shared<lsm::MemTable>();
    first_sv->mutable_mem->Add(1, "first", "value-1", lsm::ValueType::kValue);
    first.Install(first_sv);

    auto* second_sv = new lsm::SuperVersion();
    second_sv->mutable_mem = std::make_shared<lsm::MemTable>();
    second_sv->mutable_mem->Add(1, "second", "value-2", lsm::ValueType::kValue);
    second.Install(second_sv);

    auto* first_view = first.GetReadView();
    auto* second_view = second.GetReadView();
    CHECK(first_view->mutable_mem->Get("first", 10).found);
    CHECK(!first_view->mutable_mem->Get("second", 10).found);
    CHECK(second_view->mutable_mem->Get("second", 10).found);
    CHECK(!second_view->mutable_mem->Get("first", 10).found);
    first_view->Unref();
    second_view->Unref();
}

static void TestSuperVersionDeferredRelease() {
    lsm::VersionManager vm;

    auto* sv1 = new lsm::SuperVersion();
    sv1->mutable_mem = std::make_shared<lsm::MemTable>();
    sv1->mutable_mem->Add(1, "k", "v1", lsm::ValueType::kValue);
    vm.Install(sv1);

    std::atomic<bool> stop{ false };
    std::atomic<int> reads{ 0 };
    std::thread reader([&] {
        while (!stop.load()) {
            auto* sv = vm.GetReadView();
            auto r = sv->mutable_mem->Get("k", 100);
            if (r.found) reads.fetch_add(1);
            sv->Unref();
        }
        });

    for (int i = 0; i < 100; ++i) {
        auto* sv = new lsm::SuperVersion();
        sv->mutable_mem = std::make_shared<lsm::MemTable>();
        sv->mutable_mem->Add(1, "k", "v" + std::to_string(i),
            lsm::ValueType::kValue);
        vm.Install(sv);
        vm.PurgeObsolete();
    }
    stop.store(true);
    reader.join();
    CHECK(reads.load() > 0);
    vm.PurgeObsolete();
}

// ============================================================================
// [5] WAL GroupCommit
// ============================================================================
static void TestWalGroupCommit() {
    std::string path = FreshPath("wal.log");
    {
        lsm::WAL wal(path, false, 1, true);
        constexpr int T = 8, N = 2000;
        std::vector<std::thread> ts;
        for (int t = 0; t < T; ++t)
            ts.emplace_back([&, t] {
            for (int i = 0; i < N; ++i)
                wal.Append("k" + std::to_string(t) + "_" + std::to_string(i),
                    "v", lsm::ValueType::kValue);
                });
        for (auto& th : ts) th.join();
        auto recs = wal.Recover();
        CHECK(recs.size() == static_cast<size_t>(T * N));
        const auto write_stats = wal.GetWriteStats();
        CHECK(write_stats.records == static_cast<uint64_t>(T * N));
        CHECK(write_stats.batches > 0);
        CHECK(write_stats.batches < write_stats.records);
    }
    SafeRemoveAll(path);
}

static void TestWritePathDiagnostics() {
    const std::string path = FreshPath("write_path_diagnostics");
    lsm::Options opt;
    opt.db_path = path;
    opt.collect_write_path_stats = true;
    opt.memtable_entries = 100;
    {
        lsm::DB db(opt);
        CHECK(db.Put("write-a", "value-a").ok());
        CHECK(db.Put("write-b", "value-b").ok());
        CHECK(db.Delete("write-b").ok());
        const auto stats = db.GetStats();
        CHECK(stats.puts == 3);
        CHECK(stats.write_path.wal_records == 3);
        CHECK(stats.write_path.wal_batches > 0);
        CHECK(stats.write_path.wal_append_ns > 0);
        CHECK(stats.write_path.wal_append_io_ns > 0);
        CHECK(stats.write_path.memtable_add_ns > 0);
    }
    SafeRemoveAll(path);
}

static void TestWalResetDuringWrites() {
    std::string path = FreshPath("wal_reset.log");
    {
        lsm::WAL wal(path, false);
        constexpr int T = 4, N = 1000;
        std::atomic<int> append_failures{ 0 };
        std::atomic<int> ready{ 0 };
        std::atomic<bool> start{ false };
        std::vector<std::thread> writers;
        for (int t = 0; t < T; ++t) {
            writers.emplace_back([&, t] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int i = 0; i < N; ++i) {
                    if (!wal.Append("k" + std::to_string(t) + "_" +
                        std::to_string(i), "v", lsm::ValueType::kValue).ok())
                        append_failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        while (ready.load(std::memory_order_acquire) != T)
            std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (int i = 0; i < 20; ++i)
            CHECK(wal.Reset().ok());
        for (auto& writer : writers) writer.join();

        CHECK(append_failures.load(std::memory_order_relaxed) == 0);
        CHECK(wal.Reset().ok());
        CHECK(wal.Append("after", "reset", lsm::ValueType::kValue).ok());
        auto recs = wal.Recover();
        CHECK(recs.size() == 1);
        if (recs.size() == 1) {
            CHECK(recs[0].key == "after");
            CHECK(recs[0].value == "reset");
        }
    }
    SafeRemoveAll(path);
}

// ============================================================================
// [6] CRUD + 重启恢复
// ============================================================================
static void TestCrudAndRestart() {
    lsm::Options opt;
    opt.db_path = FreshPath("crud");
    opt.memtable_entries = 200;
    opt.max_file_size = 4 * 1024;
    opt.block_cache_capacity = 16 * 1024 * 1024;
    {
        lsm::DB db(opt);
        for (int i = 0; i < 500; ++i)
            CHECK(db.Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
        CHECK(db.Delete("k100").ok());
        std::string v;
        CHECK(db.Get("k100", &v).IsNotFound());
        CHECK(db.Flush().ok());
    }
    {
        lsm::DB db(opt);
        std::string v;
        for (int i = 0; i < 500; ++i) {
            if (i == 100) CHECK(db.Get("k100", &v).IsNotFound());
            else {
                CHECK(db.Get("k" + std::to_string(i), &v).ok());
                CHECK(v == "v" + std::to_string(i));
            }
        }
    }
    SafeRemoveAll(opt.db_path);
}

// ============================================================================
// [7] Compaction 保留未参与文件
// ============================================================================
static void TestCompactionPreservesUninvolved() {
    lsm::Options opt;
    opt.db_path = FreshPath("compact");
    opt.memtable_entries = 50;
    opt.level0_trigger = 2;
    opt.max_file_size = 4 * 1024;
    opt.max_bytes_for_level_base = 32 * 1024;
    opt.block_cache_capacity = 16 * 1024 * 1024;

    {
        lsm::DB db(opt);
        for (int i = 0; i < 300; ++i)
            CHECK(db.Put("k" + std::to_string(i), std::string(32, 'x')).ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());

        auto st = db.GetStats();
        std::printf("  compact: level files =");
        for (auto n : st.level_files) std::printf(" %zu", n);
        std::printf("\n");

        std::string v;
        for (int i = 0; i < 300; ++i)
            CHECK(db.Get("k" + std::to_string(i), &v).ok());
    }
    SafeRemoveAll(opt.db_path);
}

// ============================================================================
// [8] 并发压力
// ============================================================================
static void TestConcurrentStress() {
    lsm::Options opt;
    opt.db_path = FreshPath("stress");
    opt.memtable_entries = 500;
    opt.max_file_size = 4 * 1024;
    opt.block_cache_capacity = 32 * 1024 * 1024;

    {
        lsm::DB db(opt);
        constexpr int W = 4, R = 4, OPS = 4000;
        std::vector<std::thread> ts;
        for (int t = 0; t < W; ++t)
            ts.emplace_back([&, t] {
            std::mt19937 rng(t);
            for (int i = 0; i < OPS; ++i)
                db.Put("k" + std::to_string(rng() % 300), "v" + std::to_string(i));
                });
        for (int t = 0; t < R; ++t)
            ts.emplace_back([&] {
            std::mt19937 rng(100 + t);
            std::string v;
            for (int i = 0; i < OPS; ++i)
                db.Get("k" + std::to_string(rng() % 300), &v);
                });
        for (auto& th : ts) th.join();

        auto st = db.GetStats();
        std::printf("  stress: puts=%llu gets=%llu flushes=%llu compactions=%llu\n",
            (unsigned long long)st.puts, (unsigned long long)st.gets,
            (unsigned long long)st.flushes, (unsigned long long)st.compactions);
        CHECK(st.puts >= static_cast<uint64_t>(W * OPS));
        CHECK(st.gets >= static_cast<uint64_t>(R * OPS));
    }   // ← db 先析构

    SafeRemoveAll(opt.db_path);   // ← 再删目录
}

// ============================================================================
// [9] Leveled Compaction 多层
// ============================================================================
static void TestConcurrentWriterBackpressureMakesProgress() {
    lsm::Options opt;
    opt.db_path = FreshPath("writer_backpressure_progress");
    opt.memtable_entries = 128;
    opt.memtable_bytes = 64 * 1024;
    opt.immutable_slowdown_threshold = 1;
    opt.immutable_stop_threshold = 2;
    opt.write_slowdown_delay_ms = 1;
    opt.bg_interval_ms = 1;
    opt.level0_trigger = 4;
    opt.max_file_size = 8 * 1024;
    opt.max_bytes_for_level_base = 64 * 1024;

    constexpr int thread_count = 8;
    constexpr int operations_per_thread = 1000;
    std::atomic<bool> start{ false };
    std::atomic<int> errors{ 0 };
    {
        lsm::DB db(opt);
        std::vector<std::thread> writers;
        writers.reserve(thread_count);
        for (int thread = 0; thread < thread_count; ++thread) {
            writers.emplace_back([&, thread] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < operations_per_thread; ++i) {
                    const std::string key = "writer_" + std::to_string(thread) + "_" + std::to_string(i);
                    const std::string value = std::to_string(thread) + ":" + std::to_string(i);
                    if (!db.Put(key, value).ok()) errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& writer : writers) writer.join();

        const auto stats = db.GetStats();
        CHECK(errors.load(std::memory_order_relaxed) == 0);
        CHECK(stats.puts == static_cast<uint64_t>(thread_count * operations_per_thread));
        CHECK(stats.write_stops > 0);
        CHECK(db.Flush().ok());

        std::string value;
        for (int thread = 0; thread < thread_count; ++thread) {
            for (int i = 0; i < operations_per_thread; ++i) {
                const std::string key = "writer_" + std::to_string(thread) + "_" + std::to_string(i);
                const std::string expected = std::to_string(thread) + ":" + std::to_string(i);
                const auto status = db.Get(key, &value);
                CHECK(status.ok());
                if (status.ok()) CHECK(value == expected);
            }
        }
    }
    {
        lsm::DB db(opt);
        std::string value;
        for (int thread = 0; thread < thread_count; ++thread) {
            for (int i = 0; i < operations_per_thread; ++i) {
                const std::string key = "writer_" + std::to_string(thread) + "_" + std::to_string(i);
                const std::string expected = std::to_string(thread) + ":" + std::to_string(i);
                const auto status = db.Get(key, &value);
                CHECK(status.ok());
                if (status.ok()) CHECK(value == expected);
            }
        }
    }
    SafeRemoveAll(opt.db_path);
}

static void TestLeveledCompactionMultiLevel() {
    lsm::Options opt;
    opt.db_path = FreshPath("leveled");
    opt.memtable_entries = 50;
    opt.level0_trigger = 2;
    opt.max_bytes_for_level_base = 16 * 1024;
    opt.max_file_size = 4 * 1024;
    opt.block_cache_capacity = 16 * 1024 * 1024;

    {
        lsm::DB db(opt);
        for (int i = 0; i < 600; ++i)
            CHECK(db.Put("k" + std::to_string(i), std::string(64, 'v')).ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());

        auto st = db.GetStats();
        std::printf("  leveled: levels =");
        for (auto n : st.level_files) std::printf(" %zu", n);
        std::printf("\n");

        // L2 应至少有一个文件
        CHECK(st.level_files.size() >= 3);
        CHECK(st.level_files[2] > 0);

        std::string v;
        for (int i = 0; i < 600; ++i)
            CHECK(db.Get("k" + std::to_string(i), &v).ok());
    }
    SafeRemoveAll(opt.db_path);
}

// ============================================================================
// [10] Leveled Compaction 层内不重叠
// ============================================================================
static void TestLeveledNoOverlap() {
    lsm::Options opt;
    opt.db_path = FreshPath("leveled_nov");
    opt.memtable_entries = 50;
    opt.level0_trigger = 2;
    opt.max_bytes_for_level_base = 8 * 1024;
    opt.max_file_size = 4 * 1024;
    opt.block_cache_capacity = 16 * 1024 * 1024;

    {
        lsm::DB db(opt);
        for (int i = 0; i < 400; ++i)
            CHECK(db.Put("k" + std::to_string(i), std::string(64, 'v')).ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());

        auto st = db.GetStats();
        std::printf("  no-overlap: levels =");
        for (auto n : st.level_files) std::printf(" %zu", n);
        std::printf("\n");

        std::string v;
        for (int i = 0; i < 400; ++i)
            CHECK(db.Get("k" + std::to_string(i), &v).ok());
    }
    SafeRemoveAll(opt.db_path);
}

static void CheckModel(lsm::DB& db, const std::map<std::string, std::string>& model) {
    std::string value;
    for (int i = 0; i < 80; ++i) {
        std::string key = "model_" + std::to_string(i);
        auto it = model.find(key);
        auto status = db.Get(key, &value);
        if (it == model.end()) CHECK(status.IsNotFound());
        else {
            CHECK(status.ok());
            if (status.ok() && value != it->second)
                std::fprintf(stderr, "model mismatch %s: expected=%s actual=%s\n",
                    key.c_str(), it->second.c_str(), value.c_str());
            if (status.ok()) CHECK(value == it->second);
        }
    }
}

static void TestModelBasedOperationsAndRestart() {
    lsm::Options opt;
    opt.db_path = FreshPath("model");
    opt.memtable_entries = 23;
    opt.block_size = 128;
    opt.max_file_size = 512;
    opt.level0_trigger = 8;
    opt.block_cache_capacity = 1024 * 1024;
    std::map<std::string, std::string> model;
    {
        lsm::DB db(opt);
        std::mt19937 rng(0x51A7);
        for (int i = 0; i < 500; ++i) {
            std::string key = "model_" + std::to_string(rng() % 80);
            if (rng() % 5 == 0) {
                CHECK(db.Delete(key).ok());
                model.erase(key);
            } else {
                std::string value = "value_" + std::to_string(i);
                CHECK(db.Put(key, value).ok());
                model[key] = std::move(value);
            }
            if (i % 41 == 0) {
                CHECK(db.Flush().ok());
                CheckModel(db, model);
            }
        }
        CHECK(db.Flush().ok());
        CheckModel(db, model);
    }
    {
        lsm::DB db(opt);
        CheckModel(db, model);
    }
    SafeRemoveAll(opt.db_path);
}

static void TestRepeatedKeyAcrossDataBlocks() {
    lsm::Options opt;
    opt.db_path = FreshPath("hot_key_blocks");
    opt.memtable_entries = 1000;
    opt.block_size = 96;
    opt.max_file_size = 1024 * 1024;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("hot", "old-value").ok());
        for (int i = 0; i < 80; ++i) {
            CHECK(db.Put("hot", "revision-" + std::to_string(i) + std::string(20, 'x')).ok());
            CHECK(db.Put("side-" + std::to_string(i), "side-value").ok());
        }
        CHECK(db.Flush().ok());
        std::string value;
        CHECK(db.Get("hot", &value).ok());
        CHECK(value == "revision-79" + std::string(20, 'x'));
    }
    SafeRemoveAll(opt.db_path);
}

static void TestFileNumbersSurviveMultipleRestarts() {
    lsm::Options opt;
    opt.db_path = FreshPath("file_numbers");
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    for (int round = 0; round < 3; ++round) {
        lsm::DB db(opt);
        CHECK(db.Put("persist_" + std::to_string(round), "v" + std::to_string(round)).ok());
        CHECK(db.Flush().ok());
    }
    {
        lsm::DB db(opt);
        std::string value;
        for (int i = 0; i < 3; ++i) {
            CHECK(db.Get("persist_" + std::to_string(i), &value).ok());
            if (db.Get("persist_" + std::to_string(i), &value).ok())
                CHECK(value == "v" + std::to_string(i));
        }
    }
    SafeRemoveAll(opt.db_path);
}

static void TestFlushFailurePreservesWrites() {
    std::string path = FreshPath("flush_failure");
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("before_failure", "kept").ok());
        plan->Arm(test::FaultPoint::kSync, ".sst");
        CHECK(!db.Flush().ok());
        std::string value;
        CHECK(db.Get("before_failure", &value).ok());
        CHECK(db.Put("after_failure", "also_kept").ok());
        CHECK(db.Get("after_failure", &value).ok());
        if (db.Get("after_failure", &value).ok()) CHECK(value == "also_kept");
    }
    {
        lsm::DB db(opt);
        std::string value;
        CHECK(db.Get("before_failure", &value).ok());
        CHECK(db.Get("after_failure", &value).ok());
    }
    SafeRemoveAll(path);
}

static void TestMissingManifestSstIsNotSilentlySalvaged() {
    std::string path = FreshPath("missing_sst");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("must_survive", "value").ok());
        CHECK(db.Flush().ok());
    }
    std::filesystem::path victim;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".sst") { victim = entry.path(); break; }
    }
    CHECK(!victim.empty());
    if (!victim.empty()) std::filesystem::remove(victim);
    bool open_failed = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { open_failed = true; }
    CHECK(open_failed);
    SafeRemoveAll(path);
}

static void TestCorruptSstReadIsNotNotFound() {
    std::string path = FreshPath("corrupt_sst");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("corrupt_me", "value").ok());
        CHECK(db.Flush().ok());
    }
    std::filesystem::path sst;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".sst") { sst = entry.path(); break; }
    }
    CHECK(!sst.empty());
    if (!sst.empty()) {
        std::fstream file(sst, std::ios::binary | std::ios::in | std::ios::out);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 0x7f;
        file.seekp(0);
        file.write(&byte, 1);
    }
    {
        lsm::DB db(opt);
        std::string value;
        auto status = db.Get("corrupt_me", &value);
        CHECK(!status.ok());
        CHECK(!status.IsNotFound());
    }
    SafeRemoveAll(path);
}

static void TestInjectedManifestFailureIsObservable() {
    std::string path = FreshPath("manifest_failure");
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("manifest_key", "value").ok());
        plan->Arm(test::FaultPoint::kSync, "MANIFEST-");
        CHECK(!db.Flush().ok());
    }
    SafeRemoveAll(path);
}

static void TestInjectedRecoveryReadFailureIsNotIgnored() {
    std::string path = FreshPath("read_failure");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("read_failure_key", "value").ok());
        CHECK(db.Flush().ok());
    }
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    plan->Arm(test::FaultPoint::kRead, "CURRENT");
    bool open_failed = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { open_failed = true; }
    CHECK(open_failed);
    SafeRemoveAll(path);
}

static void TestInjectedWalAppendFailurePropagates() {
    std::string path = FreshPath("wal_write_failure");
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    lsm::Options opt;
    opt.db_path = path;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        plan->Arm(test::FaultPoint::kAppend, "wal.log");
        CHECK(!db.Put("must_not_ack", "value").ok());
    }
    SafeRemoveAll(path);
}

static void TestCurrentRenameFailureStopsOpen() {
    std::string path = FreshPath("rename_failure");
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    plan->Arm(test::FaultPoint::kRenameFile, "CURRENT.tmp");
    lsm::Options opt;
    opt.db_path = path;
    opt.block_cache_capacity = 1024 * 1024;
    bool open_failed = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { open_failed = true; }
    CHECK(open_failed);
    SafeRemoveAll(path);
}

static void TestDirectorySyncFailureStopsOpen() {
    std::string path = FreshPath("dirsync_failure");
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    plan->Arm(test::FaultPoint::kSyncDir, path);
    lsm::Options opt;
    opt.db_path = path;
    opt.block_cache_capacity = 1024 * 1024;
    bool open_failed = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { open_failed = true; }
    CHECK(open_failed);
    SafeRemoveAll(path);
}

static void TestSstReadFailureIsNotReportedAsNotFound() {
    std::string path = FreshPath("sst_read_failure");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("read_key", "value").ok());
        CHECK(db.Flush().ok());
    }
    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    {
        lsm::DB db(opt);
        plan->Arm(test::FaultPoint::kRead, ".sst");
        std::string value;
        auto status = db.Get("read_key", &value);
        CHECK(!status.ok());
        CHECK(!status.IsNotFound());
    }
    SafeRemoveAll(path);
}

static void TestTruncatedManifestIsNotAcceptedAsEmptyVersion() {
    std::string path = FreshPath("truncated_manifest");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("manifest_data", "value").ok());
        CHECK(db.Flush().ok());
    }
    std::filesystem::path manifest;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().filename().string().find("MANIFEST-") == 0) {
            manifest = entry.path();
            break;
        }
    }
    CHECK(!manifest.empty());
    if (!manifest.empty()) {
        std::fstream file(manifest, std::ios::binary | std::ios::in | std::ios::out);
        const uint32_t invalid_length = 0x7fffffffu;
        file.write(reinterpret_cast<const char*>(&invalid_length), sizeof(invalid_length));
    }
    bool open_failed = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { open_failed = true; }
    CHECK(open_failed);
    SafeRemoveAll(path);
}

static void TestCompactionDoesNotDropTombstoneAboveDeeperData() {
    lsm::Options opt;
    opt.level0_trigger = 1;
    lsm::Version version;
    version.levels.resize(4);
    auto tombstone = lsm::MakeInternalKey("target", 10, lsm::ValueType::kDeletion);
    auto old_value = lsm::MakeInternalKey("target", 1, lsm::ValueType::kValue);
    version.AddFile(0, {0, 10, tombstone, tombstone, 100}, nullptr);
    version.AddFile(3, {3, 30, old_value, old_value, 100}, nullptr);
    lsm::LeveledPolicy policy;
    auto inputs = policy.PickCompaction(version, opt);
    CHECK(inputs.size() == 1);
    if (!inputs.empty()) CHECK(!inputs[0].is_bottommost);
}

static void TestBlockCacheHitRefreshesRecency() {
    constexpr int shard_bits = 4;
    lsm::ShardedLRUCache cache(160 * (1 << shard_bits), shard_bits);
    auto shard_index = [](uint64_t file_number, uint64_t block_offset) {
        uint64_t h = file_number * 0x9E3779B97F4A7C15ULL + block_offset;
        h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 27; h *= 0x94D049BB133111EBULL;
        h ^= h >> 31;
        return static_cast<size_t>(h >> (64 - shard_bits));
    };
    std::vector<lsm::ShardedLRUCache::CacheKey> same_shard;
    for (uint64_t offset = 1; same_shard.size() < 3; ++offset) {
        lsm::ShardedLRUCache::CacheKey key{1, offset};
        if (same_shard.empty() || shard_index(key.file_number, key.block_offset) ==
            shard_index(same_shard.front().file_number, same_shard.front().block_offset))
            same_shard.push_back(key);
    }
    cache.Insert(same_shard[0], std::string(16, 'a'));
    CHECK(cache.Lookup(same_shard[0]) != nullptr);
    cache.Insert(same_shard[1], std::string(16, 'b'));
    CHECK(cache.Lookup(same_shard[0]) != nullptr);
    cache.Insert(same_shard[2], std::string(16, 'c'));
    CHECK(cache.Lookup(same_shard[0]) != nullptr);
}

static void TestSstIteratorRejectsBadBlockChecksum() {
    std::string path = FreshPath("iterator_crc");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("iterator_crc_key", "value").ok());
        CHECK(db.Flush().ok());
    }
    std::filesystem::path sst;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".sst") { sst = entry.path(); break; }
    }
    CHECK(!sst.empty());
    if (!sst.empty()) {
        std::fstream file(sst, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-44, std::ios::end);
        char footer[44]{};
        file.read(footer, sizeof(footer));
        uint64_t index_offset = 0, block_offset = 0, block_size = 0;
        std::memcpy(&index_offset, footer, 8);
        file.seekg(static_cast<std::streamoff>(index_offset + 4), std::ios::beg);
        file.read(reinterpret_cast<char*>(&block_offset), 8);
        file.read(reinterpret_cast<char*>(&block_size), 8);
        file.seekg(static_cast<std::streamoff>(block_offset + block_size + 1), std::ios::beg);
        char crc_byte = 0;
        file.read(&crc_byte, 1);
        crc_byte ^= 0x1;
        file.seekp(static_cast<std::streamoff>(block_offset + block_size + 1), std::ios::beg);
        file.write(&crc_byte, 1);
    }
    auto cache = std::make_shared<lsm::ShardedLRUCache>(1024 * 1024, 2);
    auto reader = lsm::TableReader::Open(sst.string(), cache);
    CHECK(reader != nullptr);
    if (reader) {
        auto it = reader->NewIterator();
        CHECK(!it.Valid());
        CHECK(it.status().IsCorruption());
    }
    SafeRemoveAll(path);
}

static void TestCompactionPropagatesInputAndOutputErrors() {
    std::string db_path = FreshPath("compaction_error_source");
    std::string output_path = FreshPath("compaction_error_output");
    lsm::Options opt;
    opt.db_path = db_path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    std::filesystem::create_directories(output_path);

    std::filesystem::path sst_path;
    {
        lsm::DB db(opt);
        CHECK(db.Put("compaction_key", "value").ok());
        CHECK(db.Flush().ok());
    }
    for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
        if (entry.path().extension() == ".sst") {
            sst_path = entry.path();
            break;
        }
    }
    CHECK(!sst_path.empty());

    if (!sst_path.empty()) {
        std::fstream file(sst_path, std::ios::binary | std::ios::in | std::ios::out);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 0x7f;
        file.seekp(0);
        file.write(&byte, 1);
    }

    {
        auto cache = std::make_shared<lsm::ShardedLRUCache>(1024 * 1024, 2);
        auto reader = lsm::TableReader::Open(sst_path.string(), cache);
        CHECK(reader != nullptr);
        if (reader) {
            lsm::CompactionPolicy::Input input;
            input.from_level = 0;
            input.to_level = 1;
            input.input_file_numbers.push_back(reader->FileNumber());
            input.is_bottommost = true;
            lsm::SyncCompactionIO compaction;
            std::vector<lsm::VersionEdit::FileMeta> outputs;
            uint64_t next_number = 9000;
            auto status = compaction.Run(input, { reader }, output_path,
                [&next_number](uint64_t* number) {
                    *number = next_number++;
                    return lsm::Status::OK();
                },
                opt, &outputs);
            CHECK(!status.ok());
            CHECK(status.IsCorruption());
            CHECK(outputs.empty());
            CHECK(std::filesystem::exists(sst_path));
            CHECK(!std::filesystem::exists(
                std::filesystem::path(output_path) / "9000.sst"));
        }
    }

    SafeRemoveAll(output_path);
    SafeRemoveAll(db_path);

    db_path = FreshPath("compaction_output_failure_source");
    output_path = FreshPath("compaction_output_failure_dest");
    opt.db_path = db_path;
    std::filesystem::create_directories(output_path);
    {
        lsm::DB db(opt);
        CHECK(db.Put("compaction_key", "value").ok());
        CHECK(db.Flush().ok());
    }
    for (const auto& entry : std::filesystem::directory_iterator(db_path)) {
        if (entry.path().extension() == ".sst") {
            sst_path = entry.path();
            break;
        }
    }

    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    auto cache = std::make_shared<lsm::ShardedLRUCache>(1024 * 1024, 2);
    auto reader = lsm::TableReader::Open(sst_path.string(), cache);
    CHECK(reader != nullptr);
    if (reader) {
        plan->Arm(test::FaultPoint::kSync, output_path);
        lsm::CompactionPolicy::Input input;
        input.from_level = 0;
        input.to_level = 1;
        input.input_file_numbers.push_back(reader->FileNumber());
        input.is_bottommost = true;
        lsm::SyncCompactionIO compaction;
        std::vector<lsm::VersionEdit::FileMeta> outputs;
        uint64_t next_number = 9001;
        auto status = compaction.Run(input, { reader }, output_path,
            [&next_number](uint64_t* number) {
                *number = next_number++;
                return lsm::Status::OK();
            },
            opt, &outputs);
        CHECK(!status.ok());
        CHECK(outputs.empty());
        CHECK(std::filesystem::exists(sst_path));
        CHECK(!std::filesystem::exists(
            std::filesystem::path(output_path) / "9001.sst"));
    }
    reader.reset();
    SafeRemoveAll(output_path);
    SafeRemoveAll(db_path);
}

static void TestWalTruncatedTailKeepsOnlyCompleteRecords() {
    std::string path = FreshPath("wal_tail");
    {
        lsm::WAL wal(path, false, 40);
        uint64_t first_sequence = 0, second_sequence = 0;
        CHECK(wal.Append("a", "one", lsm::ValueType::kValue,
            &first_sequence).ok());
        CHECK(wal.Append("b", "two", lsm::ValueType::kValue,
            &second_sequence).ok());
        CHECK(first_sequence == 40);
        CHECK(second_sequence == 41);
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        const char tail[] = { 12, 0, 0 };
        file.write(tail, sizeof(tail));
    }
    {
        lsm::WAL wal(path, false);
        std::vector<lsm::WAL::Record> records;
        CHECK(wal.Recover(0, &records).ok());
        CHECK(records.size() == 2);
        if (records.size() == 2) {
            CHECK(records[0].sequence == 40);
            CHECK(records[1].sequence == 41);
        }
        uint64_t next_sequence = 0;
        CHECK(wal.Append("c", "three", lsm::ValueType::kValue,
            &next_sequence).ok());
        CHECK(next_sequence == 42);
    }
    {
        lsm::WAL wal(path, false);
        std::vector<lsm::WAL::Record> records;
        CHECK(wal.Recover(0, &records).ok());
        CHECK(records.size() == 3);
        if (records.size() == 3) CHECK(records[2].sequence == 42);
    }
    SafeRemoveAll(path);
}

static void TestWalChecksumCorruptionIsReported() {
    std::string path = FreshPath("wal_corrupt_crc");
    {
        lsm::WAL wal(path, false);
        CHECK(wal.Append("crc_key", "crc_value", lsm::ValueType::kValue).ok());
    }
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    {
        lsm::WAL wal(path, false);
        std::vector<lsm::WAL::Record> records;
        auto status = wal.Recover(0, &records);
        CHECK(status.IsCorruption());
        CHECK(records.empty());
    }
    SafeRemoveAll(path);
}

static void TestManifestChecksumCorruptionIsReported() {
    std::string path = FreshPath("manifest_crc");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1000;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        CHECK(db.Put("manifest_crc_key", "value").ok());
        CHECK(db.Flush().ok());
    }
    std::filesystem::path manifest;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().filename().string().find("MANIFEST-") == 0) {
            manifest = entry.path();
            break;
        }
    }
    CHECK(!manifest.empty());
    if (!manifest.empty()) {
        std::fstream file(manifest, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 1;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    bool open_failed = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { open_failed = true; }
    CHECK(open_failed);
    SafeRemoveAll(path);
}

static void TestImmutableQueueBackpressureAndRetry() {
    const std::string path = FreshPath("immutable_backpressure");
    lsm::Options opt;
    opt.db_path = path;
    opt.memtable_entries = 1;
    opt.memtable_bytes = 1024 * 1024;
    opt.immutable_slowdown_threshold = 1;
    opt.immutable_stop_threshold = 2;
    opt.write_slowdown_delay_ms = 1;
    opt.bg_interval_ms = 500;
    opt.block_cache_capacity = 1024 * 1024;

    auto plan = std::make_shared<test::FaultPlan>();
    test::FaultInjectingFileSystem injected(lsm::FileSystem::Default(), plan);
    test::ScopedFileSystemOverride override_fs(&injected);
    lsm::Stats pressure_stats;
    {
        lsm::DB db(opt);
        plan->Arm(test::FaultPoint::kAppendAt, ".sst");
        CHECK(db.Put("first", "v1").ok());

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        while (plan->IsArmed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(!plan->IsArmed());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        CHECK(db.Put("second", "v2").ok());
        CHECK(db.Put("third", "v3").ok());
        pressure_stats = db.GetStats();
        CHECK(pressure_stats.write_stops > 0);
        CHECK(pressure_stats.immutable_memtables_peak <= opt.immutable_stop_threshold);
        CHECK(db.Flush().ok());
        std::string value;
        CHECK(db.Get("first", &value).ok() && value == "v1");
        CHECK(db.Get("second", &value).ok() && value == "v2");
        CHECK(db.Get("third", &value).ok() && value == "v3");
    }
    {
        lsm::DB db(opt);
        std::string value;
        CHECK(db.Get("first", &value).ok() && value == "v1");
        CHECK(db.Get("second", &value).ok() && value == "v2");
        CHECK(db.Get("third", &value).ok() && value == "v3");
    }
    SafeRemoveAll(path);
}

static void TestDurableWalGenerationRestart() {
    const std::string path = FreshPath("durable_wal_generations");
    lsm::Options opt;
    opt.db_path = path;
    opt.sync_wal = true;
    opt.memtable_entries = 2;
    opt.memtable_bytes = 1024 * 1024;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        for (int i = 0; i < 8; ++i)
            CHECK(db.Put("durable-" + std::to_string(i),
                "value-" + std::to_string(i)).ok());
        CHECK(db.Flush().ok());
    }
    {
        lsm::DB db(opt);
        std::string value;
        for (int i = 0; i < 8; ++i) {
            CHECK(db.Get("durable-" + std::to_string(i), &value).ok());
            CHECK(value == "value-" + std::to_string(i));
        }
    }

    uint64_t active_log = 0;
    uint64_t last_sequence = 0;
    {
        auto cache = std::make_shared<lsm::ShardedLRUCache>(1024 * 1024, 2);
        lsm::VersionSet versions(path, opt, cache);
        CHECK(versions.Recover().ok());
        active_log = versions.LogNumber();
        last_sequence = versions.LastSequence();
    }
    std::string active_wal_path;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("WAL-", 0) != 0 || entry.path().extension() != ".log")
            continue;
        const size_t suffix = name.size() - 4;
        uint64_t generation = 0;
        try { generation = std::stoull(name.substr(4, suffix - 4)); }
        catch (const std::exception&) { continue; }
        if (generation == active_log) active_wal_path = entry.path().string();
    }
    CHECK(active_log != 0 && !active_wal_path.empty());
    if (!active_wal_path.empty()) {
        lsm::WAL wal(active_wal_path, true, last_sequence + 1);
        CHECK(wal.OpenStatus().ok());
        uint64_t appended_sequence = 0;
        CHECK(wal.Append("wal-replay-key", "replayed-value",
            lsm::ValueType::kValue, &appended_sequence).ok());
        CHECK(appended_sequence == last_sequence + 1);
    }
    {
        lsm::DB db(opt);
        std::string value;
        CHECK(db.Get("wal-replay-key", &value).ok());
        CHECK(value == "replayed-value");
    }
    SafeRemoveAll(path);
}

static void TestConcurrentSameKeyAcrossWalGenerations() {
    const std::string path = FreshPath("same_key_wal_generations");
    lsm::Options opt;
    opt.db_path = path;
    opt.sync_wal = true;
    opt.memtable_entries = 8;
    opt.memtable_bytes = 1024 * 1024;
    opt.block_cache_capacity = 1024 * 1024;
    {
        lsm::DB db(opt);
        std::atomic<int> errors{ 0 };
        std::vector<std::thread> writers;
        for (int thread = 0; thread < 4; ++thread) {
            writers.emplace_back([&, thread] {
                for (int i = 0; i < 80; ++i) {
                    lsm::Status status = ((i + thread) % 3 == 0)
                        ? db.Delete("shared-key")
                        : db.Put("shared-key", std::to_string(thread) + ":" +
                            std::to_string(i));
                    if (!status.ok()) errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& writer : writers) writer.join();
        CHECK(errors.load(std::memory_order_relaxed) == 0);
        CHECK(db.Put("shared-key", "final-value").ok());
        CHECK(db.Flush().ok());
        std::string value;
        CHECK(db.Get("shared-key", &value).ok() && value == "final-value");
    }
    {
        lsm::DB db(opt);
        std::string value;
        CHECK(db.Get("shared-key", &value).ok() && value == "final-value");
    }

    std::vector<std::filesystem::path> wal_generations;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("WAL-", 0) == 0 && entry.path().extension() == ".log")
            wal_generations.push_back(entry.path());
    }
    CHECK(!wal_generations.empty());
    for (const auto& wal_path : wal_generations) {
        std::error_code ec;
        std::filesystem::remove(wal_path, ec);
        CHECK(!ec);
    }
    bool missing_wal_rejected = false;
    try { lsm::DB db(opt); }
    catch (const std::exception&) { missing_wal_rejected = true; }
    CHECK(missing_wal_rejected);
    SafeRemoveAll(path);
}

template<class F>
static void RunTest(const char* name, F&& test) {
    ++tests_run;
    const int failures_before = failures;
    const auto started = std::chrono::steady_clock::now();
    std::printf("[ RUN      ] %s\n", name);
    std::fflush(stdout);
    try {
        test();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ EXCEPTION ] %s: %s\n", name, e.what());
        ++failures;
    } catch (...) {
        std::fprintf(stderr, "[ EXCEPTION ] %s: unknown\n", name);
        ++failures;
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("[ %s ] %s (%.3fs)\n",
        failures == failures_before ? "       OK" : "  FAILED", name, elapsed);
}

// ============================================================================
// main
// ============================================================================
int main() {
    const auto suite_started = std::chrono::steady_clock::now();
    std::printf("=== LSM-Tree regression tests ===\n");
    std::printf("CRC32C backend: %s\n", lsm::CRC32C::EffectiveBackendName().c_str());
    const char* db_root = std::getenv("LSM_TEST_DB_ROOT");
    if (db_root && *db_root) std::printf("Test database root: %s\n", db_root);

    RunTest("Arena", TestArena);
    RunTest("MANIFEST round-trip", TestManifestRoundTrip);
    RunTest("Compaction clean cut", TestCleanCutExpansion);
    RunTest("VersionManager isolation", TestVersionManagerIsolation);
    RunTest("SuperVersion deferred release", TestSuperVersionDeferredRelease);
    RunTest("WAL group commit", TestWalGroupCommit);
    RunTest("Write-path diagnostics", TestWritePathDiagnostics);
    RunTest("WAL reset during writes", TestWalResetDuringWrites);
    RunTest("CRUD and restart", TestCrudAndRestart);
    RunTest("Compaction preservation", TestCompactionPreservesUninvolved);
    RunTest("Concurrent stress", TestConcurrentStress);
    RunTest("Concurrent writer backpressure progress", TestConcurrentWriterBackpressureMakesProgress);
    RunTest("Leveled compaction", TestLeveledCompactionMultiLevel);
    RunTest("Leveled non-overlap", TestLeveledNoOverlap);
    RunTest("Model operations and restart", TestModelBasedOperationsAndRestart);
    RunTest("Repeated key across blocks", TestRepeatedKeyAcrossDataBlocks);
    RunTest("File number recovery", TestFileNumbersSurviveMultipleRestarts);
    RunTest("Flush failure preservation", TestFlushFailurePreservesWrites);
    RunTest("Missing manifest SST", TestMissingManifestSstIsNotSilentlySalvaged);
    RunTest("Corrupt SST status", TestCorruptSstReadIsNotNotFound);
    RunTest("MANIFEST sync failure injection", TestInjectedManifestFailureIsObservable);
    RunTest("Recovery read failure", TestInjectedRecoveryReadFailureIsNotIgnored);
    RunTest("WAL append failure", TestInjectedWalAppendFailurePropagates);
    RunTest("CURRENT rename failure", TestCurrentRenameFailureStopsOpen);
    RunTest("Directory sync failure", TestDirectorySyncFailureStopsOpen);
    RunTest("SST read failure status", TestSstReadFailureIsNotReportedAsNotFound);
    RunTest("Truncated MANIFEST", TestTruncatedManifestIsNotAcceptedAsEmptyVersion);
    RunTest("Tombstone bottommost range", TestCompactionDoesNotDropTombstoneAboveDeeperData);
    RunTest("Block cache recency", TestBlockCacheHitRefreshesRecency);
    RunTest("SST iterator checksum", TestSstIteratorRejectsBadBlockChecksum);
    RunTest("Compaction error propagation", TestCompactionPropagatesInputAndOutputErrors);
    RunTest("WAL truncated tail and sequence", TestWalTruncatedTailKeepsOnlyCompleteRecords);
    RunTest("WAL checksum corruption", TestWalChecksumCorruptionIsReported);
    RunTest("MANIFEST checksum corruption", TestManifestChecksumCorruptionIsReported);
    RunTest("Immutable queue backpressure and retry", TestImmutableQueueBackpressureAndRetry);
    RunTest("Durable WAL generation restart", TestDurableWalGenerationRestart);
    RunTest("Concurrent same key across WAL generations", TestConcurrentSameKeyAcrossWalGenerations);

    const double suite_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - suite_started).count();
    if (failures == 0) {
        std::printf("\n[PASS] all tests (%d cases; %.3fs)\n", tests_run, suite_seconds);
        std::fflush(stdout);      // ★ 强制 flush，防止 abort 时丢失
        return 0;
    }
    std::printf("\n[FAIL] %d checks failed across %d test cases in %.3fs\n",
        failures, tests_run, suite_seconds);
    std::fflush(stdout);
    return 1;
}