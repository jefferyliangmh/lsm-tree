#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/bloom.h"
#include "lsm/compaction.h"
#include "lsm/db.h"
#include "lsm/flat_memtable.h"
#include "lsm/memtable.h"
#include "lsm/metrics.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static std::string FreshPath(const char* name) {
    std::filesystem::path path;
#ifdef _WIN32
    char tmp[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, tmp);
    path = std::filesystem::path(tmp) / (std::string("lsm_adv_") + name);
#else
    path = std::filesystem::path("/tmp") / (std::string("lsm_adv_") + name);
#endif
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path.string();
}

static lsm::Options BaseOptions(const char* name) {
    lsm::Options opt;
    opt.db_path = FreshPath(name);
    opt.memtable_entries = 32;
    opt.memtable_bytes = 1ull << 20;
    opt.max_file_size = 1ull << 20;
    opt.level0_trigger = 2;
    opt.tiered_fanout = 2;
    opt.max_bytes_for_level_base = 64 * 1024;
    opt.block_cache_capacity = 8ull << 20;
    opt.block_cache_shard_bits = 2;
    opt.bg_interval_ms = 20;
    opt.sync_wal = false;
    return opt;
}

static void TestBranchlessLowerBound() {
    // 构造最小记录：u32 klen | u32 vlen | key | value，key 是内部键。
    std::vector<std::string> storage;
    std::vector<const char*> keys;
    auto push = [&](const std::string& user, uint64_t seq) {
        std::string ikey = lsm::MakeInternalKey(user, seq, lsm::ValueType::kValue);
        std::string rec(8 + ikey.size(), '\0');
        uint32_t klen = static_cast<uint32_t>(ikey.size());
        uint32_t vlen = 1;
        std::memcpy(rec.data(), &klen, 4);
        std::memcpy(rec.data() + 4, &vlen, 4);
        std::memcpy(rec.data() + 8, ikey.data(), ikey.size());
        storage.push_back(std::move(rec));
    };
    push("a", 3); push("a", 1); push("b", 5); push("c", 2); push("m", 9); push("z", 1);
    std::sort(storage.begin(), storage.end(), [](const std::string& a, const std::string& b) {
        lsm::Slice ka(a.data() + 8, a.size() - 8);
        lsm::Slice kb(b.data() + 8, b.size() - 8);
        return lsm::CompareInternal(ka, kb) < 0;
    });
    for (auto& rec : storage) keys.push_back(rec.data());

    auto expect = [&](const std::string& user, uint64_t seq) {
        std::string target = lsm::MakeInternalKey(user, seq, lsm::ValueType::kValue);
        size_t idx = lsm::AppendOnlySimdTable::BranchlessLowerBound(keys, lsm::Slice(target));
        size_t linear = 0;
        while (linear < keys.size() &&
            lsm::CompareInternal(lsm::Slice(keys[linear] + 8,
                *reinterpret_cast<const uint32_t*>(keys[linear])), lsm::Slice(target)) < 0)
            ++linear;
        CHECK(idx == linear);
    };
    expect("a", 100);
    expect("a", 2);
    expect("b", 5);
    expect("m", 1);
    expect("z", 0);
    expect("zzz", 1);
}

static void TestFlatMatchesSkipList() {
    lsm::MemTable skiplist(lsm::MemTableType::kSkipList);
    lsm::MemTable flat(lsm::MemTableType::kAppendOnlySimd);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, 80);
    std::uniform_int_distribution<int> op_dist(0, 9);
    uint64_t seq = 1;
    for (int i = 0; i < 500; ++i) {
        std::string key = "k" + std::to_string(key_dist(rng));
        if (op_dist(rng) == 0) {
            skiplist.Add(seq, key, "", lsm::ValueType::kDeletion);
            flat.Add(seq, key, "", lsm::ValueType::kDeletion);
        } else {
            std::string value = "v" + std::to_string(seq);
            skiplist.Add(seq, key, value, lsm::ValueType::kValue);
            flat.Add(seq, key, value, lsm::ValueType::kValue);
        }
        ++seq;
        if (i % 7 == 0) {
            uint64_t snap = seq;
            for (int k = 0; k < 40; ++k) {
                std::string key_q = "k" + std::to_string(k);
                auto a = skiplist.Get(key_q, snap);
                auto b = flat.Get(key_q, snap);
                CHECK(a.found == b.found);
                CHECK(a.type == b.type);
                CHECK(a.value == b.value);
            }
        }
    }
    auto it = flat.NewIterator();
    std::string prev;
    bool have = false;
    int count = 0;
    for (; it.Valid(); it.Next()) {
        std::string key(it.Key().data(), it.Key().size());
        if (have) CHECK(lsm::CompareInternal(lsm::Slice(prev), lsm::Slice(key)) < 0);
        prev = key;
        have = true;
        ++count;
    }
    CHECK(count == static_cast<int>(flat.Size()));
    CHECK(flat.Size() == skiplist.Size());
}

static void TestAppendOnlyDatabase() {
    auto opt = BaseOptions("simd_db");
    opt.memtable_type = lsm::MemTableType::kAppendOnlySimd;
    opt.compaction_style = lsm::CompactionStyle::kLeveled;
    {
        lsm::DB db(opt);
        for (int i = 0; i < 200; ++i)
            CHECK(db.Put("key-" + std::to_string(i), "val-" + std::to_string(i)).ok());
        CHECK(db.Delete("key-3").ok());
        CHECK(db.Put("key-3", "replaced").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());
        std::string v;
        CHECK(db.Get("key-3", &v).ok());
        CHECK(v == "replaced");
        CHECK(db.Get("missing", &v).IsNotFound());
        CHECK(db.GetStats().memtable_type == "append-only-simd");
    }
    {
        lsm::DB db(opt);
        std::string v;
        CHECK(db.Get("key-3", &v).ok());
        CHECK(v == "replaced");
        CHECK(db.Get("key-199", &v).ok());
    }
    std::filesystem::remove_all(opt.db_path);
}

static void TestPolicies() {
    lsm::Options opt;
    opt.level0_trigger = 2;
    opt.tiered_fanout = 3;
    opt.hybrid_write_heavy_ratio = 4.0;
    opt.hybrid_leveled_from = 2;
    opt.max_bytes_for_level_base = 100;
    opt.max_bytes_for_level_multiplier = 10;

    auto IK = [](const char* k) {
        return lsm::MakeInternalKey(k, 1, lsm::ValueType::kValue);
    };
    lsm::Version v;
    v.levels.resize(4);
    lsm::VersionEdit::FileMeta a{ 0, 10, IK("a"), IK("c"), 50 };
    lsm::VersionEdit::FileMeta b{ 0, 11, IK("b"), IK("d"), 50 };
    lsm::VersionEdit::FileMeta c{ 0, 12, IK("m"), IK("z"), 50 };
    lsm::VersionEdit::FileMeta l1{ 1, 3, IK("a"), IK("d"), 80 };
    v.AddFile(0, a, nullptr);
    v.AddFile(0, b, nullptr);
    v.AddFile(0, c, nullptr);
    v.AddFile(1, l1, nullptr);

    lsm::TieredPolicy tiered;
    auto tiered_in = tiered.PickCompaction(v, opt);
    CHECK(tiered_in.size() == 1);
    CHECK(tiered_in[0].from_level == 0);
    CHECK(tiered_in[0].to_level == 1);
    CHECK(tiered_in[0].input_file_numbers.size() == 3);
    CHECK(!tiered_in[0].is_bottommost);
    bool pulled_l1 = false;
    for (auto n : tiered_in[0].input_file_numbers) if (n == 3) pulled_l1 = true;
    CHECK(!pulled_l1);

    lsm::HybridPolicy hybrid;
    opt.observed_write_read_ratio = 20;
    auto heavy = hybrid.PickCompaction(v, opt);
    CHECK(heavy.size() == 1);
    CHECK(heavy[0].input_file_numbers.size() == 3);
    CHECK(hybrid.Name() == "hybrid");

    opt.observed_write_read_ratio = 0.1;
    auto light = hybrid.PickCompaction(v, opt);
    CHECK(light.size() == 1);
    bool light_has_l1 = false;
    for (auto n : light[0].input_file_numbers) if (n == 3) light_has_l1 = true;
    CHECK(light_has_l1);
}

static void TestTieredNoResurrection() {
    auto opt = BaseOptions("tiered_del");
    opt.compaction_style = lsm::CompactionStyle::kSizeTiered;
    opt.memtable_entries = 100000;
    {
        lsm::DB db(opt);
        CHECK(db.Put("k", "v1").ok());
        CHECK(db.Put("a", "1").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Put("b", "2").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());
        CHECK(db.Delete("k").ok());
        CHECK(db.Put("c", "3").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Put("d", "4").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());
        std::string v;
        CHECK(db.Get("k", &v).IsNotFound());
        CHECK(db.Get("a", &v).ok());
        CHECK(v == "1");
        CHECK(db.Put("k", "v2").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Get("k", &v).ok());
        CHECK(v == "v2");
        CHECK(db.CompactionPolicyName() == "size-tiered");
        auto st = db.GetStats();
        CHECK(st.compaction_io == "pipelined-async");
        CHECK(st.flushes >= 1);
    }
    {
        lsm::DB db(opt);
        std::string v;
        CHECK(db.Get("k", &v).ok());
        CHECK(v == "v2");
        CHECK(db.Get("a", &v).ok());
    }
    std::filesystem::remove_all(opt.db_path);
}

static void TestHybridAndAdaptiveAndLearned() {
    auto opt = BaseOptions("hybrid");
    opt.compaction_style = lsm::CompactionStyle::kHybrid;
    opt.memtable_type = lsm::MemTableType::kAppendOnlySimd;
    opt.filter_kind = lsm::FilterKind::kLearnedBloom;
    opt.bloom_bits_per_key = 6;
    opt.memtable_entries = 100000;
    {
        lsm::DB db(opt);
        CHECK(db.Put("aa", "1").ok());
        CHECK(db.Put("mm", "2").ok());
        CHECK(db.Put("zz", "3").ok());
        CHECK(db.Flush().ok());
        std::string v;
        CHECK(db.Get("aa", &v).ok());
        CHECK(v == "1");
        CHECK(db.Get("mn", &v).IsNotFound());
        auto st = db.GetStats();
        CHECK(st.filter_kind == "learned-bloom");
        CHECK(st.filter_rejects >= 1);
        CHECK(db.CompactionPolicyName() == "hybrid");
    }
    std::filesystem::remove_all(opt.db_path);

    auto adapt = BaseOptions("adapt");
    adapt.filter_kind = lsm::FilterKind::kAdaptiveBloom;
    adapt.bloom_bits_per_key = 4;
    adapt.bloom_bits_min = 4;
    adapt.bloom_bits_max = 16;
    adapt.bloom_target_fpr = 0.01;
    adapt.memtable_entries = 100000;
    adapt.use_bloom = true;
    // 后台循环也会 RetuneFilter 并清零窗口。探针期间关掉它，计数才稳定。
    adapt.bg_interval_ms = 600000;
    {
        lsm::DB db(adapt);
        CHECK(db.Put("a", "left").ok());
        CHECK(db.Put("z", "right").ok());
        for (int i = 0; i < 30; ++i)
            CHECK(db.Put("m" + std::to_string(i), "v").ok());
        CHECK(db.Flush().ok());
        const int before = db.TunedBloomBits();
        std::string v;
        for (int i = 0; i < 800; ++i)
            CHECK(db.Get("m-miss-" + std::to_string(i), &v).IsNotFound());
        auto st = db.GetStats();
        CHECK(st.filter_lookups >= 64);
        CHECK(st.filter_false_positives + st.filter_rejects >= 64);
        db.RetuneFilter();
        CHECK(db.TunedBloomBits() > before);
    }
    std::filesystem::remove_all(adapt.db_path);
}

static void TestLearnedWrapperUnit() {
    lsm::BloomBuilder builder(8);
    std::string blob;
    std::vector<std::pair<size_t, size_t>> spans;
    auto add = [&](const std::string& key) {
        spans.push_back({ blob.size(), key.size() });
        blob.append(key);
        builder.Add(lsm::Slice(key));
    };
    add("aa");
    add("ab");
    add("ba");
    std::string classic = builder.Finish();
    std::string wrapped = lsm::WrapLearnedBloom(classic, blob, spans);
    CHECK(wrapped.size() > classic.size());
    lsm::BloomFilter filter(wrapped);
    CHECK(filter.MightContain(lsm::Slice(std::string("aa"))));
    CHECK(!filter.MightContain(lsm::Slice(std::string("zz"))));
    lsm::BloomFilter classic_filter(classic);
    CHECK(classic_filter.MightContain(lsm::Slice(std::string("aa"))));
}

static void TestSchedulerBackpressureProgress() {
    auto opt = BaseOptions("sched");
    opt.memtable_entries = 40;
    opt.memtable_bytes = 256 * 1024;
    opt.level0_trigger = 2;
    opt.immutable_slowdown_threshold = 2;
    opt.immutable_stop_threshold = 4;
    opt.flush_threads = 1;
    opt.compaction_threads = 1;
    opt.max_compaction_backlog = 1;
    lsm::EngineCounters::Reset();
    {
        lsm::DB db(opt);
        std::atomic<bool> failed{ false };
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < 150; ++i) {
                    auto st = db.Put("k" + std::to_string(t) + "-" + std::to_string(i),
                        std::string(32, 'x'));
                    if (!st.ok()) failed.store(true);
                }
            });
        }
        for (auto& th : writers) th.join();
        CHECK(!failed.load());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());
        std::string v;
        CHECK(db.Get("k0-0", &v).ok());
        CHECK(db.Get("k3-149", &v).ok());
        auto st = db.GetStats();
        CHECK(st.scheduler_flush_tasks >= 1);
        CHECK(st.flushes >= 1);
        CHECK(st.user_write_bytes > 0);
        CHECK(st.flush_bytes > 0);
    }
    std::filesystem::remove_all(opt.db_path);
}

static void TestPipelinedCompactionBytes() {
    auto opt = BaseOptions("pipe");
    opt.memtable_entries = 100000;
    opt.pipelined_compaction_io = true;
    lsm::EngineCounters::Reset();
    {
        lsm::DB db(opt);
        for (int i = 0; i < 80; ++i)
            CHECK(db.Put(std::to_string(i), std::string(64, 'a' + (i % 26))).ok());
        CHECK(db.Flush().ok());
        for (int i = 80; i < 160; ++i)
            CHECK(db.Put(std::to_string(i), "z").ok());
        CHECK(db.Flush().ok());
        CHECK(db.Compact().ok());
        std::string v;
        CHECK(db.Get("0", &v).ok());
        CHECK(db.Get("159", &v).ok());
        auto st = db.GetStats();
        CHECK(st.prefetch_blocks > 0);
        CHECK(st.compaction_output_bytes > 0);
        const double write_amp = static_cast<double>(st.flush_bytes + st.compaction_output_bytes) /
            static_cast<double>(st.user_write_bytes);
        CHECK(write_amp >= 1.0);
    }
    std::filesystem::remove_all(opt.db_path);
}

int main() {
    TestBranchlessLowerBound();
    TestFlatMatchesSkipList();
    TestAppendOnlyDatabase();
    TestPolicies();
    TestTieredNoResurrection();
    TestHybridAndAdaptiveAndLearned();
    TestLearnedWrapperUnit();
    TestSchedulerBackpressureProgress();
    TestPipelinedCompactionBytes();
    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("advanced tests passed\n");
    return 0;
}
