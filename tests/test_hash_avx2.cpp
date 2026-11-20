#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/bloom.h"
#include "lsm/hash_avx2.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static void TestScalarAVX2Consistency() {
    if (!lsm::BloomHash::AVX2Available()) {
        std::printf("  (AVX2 not available, skipping)\n");
        return;
    }
    std::mt19937_64 rng(0xdeadbeef);
    std::vector<size_t> sizes = {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33,
                                 63, 64, 65, 100, 255, 256, 1000, 4096};
    for (size_t n : sizes) {
        std::string data(n, '\0');
        for (size_t i = 0; i < n; ++i) data[i] = static_cast<char>(rng() & 0xFF);

        lsm::BloomHash::Configure(lsm::BloomHash::Backend::kSoftware);
        uint64_t sw = lsm::BloomHash::HashOne(data.data(), n);

        lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAVX2);
        uint64_t hw = lsm::BloomHash::HashOne(data.data(), n);

        CHECK(sw == hw);
    }
    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAuto);
}

static void TestBatchVsSingle() {
    std::mt19937_64 rng(42);
    constexpr size_t N = 100;
    std::vector<std::string> keys(N);
    for (auto& k : keys) {
        size_t len = rng() % 64;
        k.resize(len);
        for (size_t i = 0; i < len; ++i) k[i] = static_cast<char>(rng() & 0xFF);
    }

    std::vector<const char*> ptrs(N);
    std::vector<size_t> lens(N);
    for (size_t i = 0; i < N; ++i) {
        ptrs[i] = keys[i].data();
        lens[i] = keys[i].size();
    }
    std::vector<uint64_t> batch(N);

    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kSoftware);
    lsm::BloomHash::HashBatch(ptrs.data(), lens.data(), N, batch.data());
    for (size_t i = 0; i < N; ++i)
        CHECK(batch[i] == lsm::BloomHash::HashOne(keys[i].data(), keys[i].size()));

    if (lsm::BloomHash::AVX2Available()) {
        lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAVX2);
        std::vector<uint64_t> avx(N);
        lsm::BloomHash::HashBatch(ptrs.data(), lens.data(), N, avx.data());
        for (size_t i = 0; i < N; ++i)
            CHECK(avx[i] == batch[i]);
    }
    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAuto);
}

//static void TestBloomFilter() {
//    lsm::BloomBuilder b(10);
//    std::vector<std::string> storage;
//    std::vector<lsm::Slice> keys;
//
//    for (int i = 0; i < 1000; ++i) {
//        storage.push_back("key_" + std::to_string(i));
//        keys.push_back(lsm::Slice(storage.back()));
//    }
//    b.AddBatch(keys);
//    std::string data = b.Finish();
//
//    lsm::BloomFilter filter(data);
//
//    // 无假阴性
//    for (auto& s : storage)
//        CHECK(filter.MightContain(lsm::Slice(s)));
//
//    // 假阳性率
//    int fp = 0;
//    for (int i = 0; i < 1000; ++i) {
//        std::string m = "missing_" + std::to_string(i);
//        if (filter.MightContain(lsm::Slice(m))) ++fp;
//    }
//    std::printf("  Bloom false positives: %d/1000 (%.1f%%)\n", fp, fp / 10.0);
//    CHECK(fp < 50);
//}

static void TestBloomFilter() {
    lsm::BloomBuilder b(10);
    std::vector<std::string> storage;
    storage.reserve(1000);          // ★ 关键：预分配，永不 reallocate
    std::vector<lsm::Slice> keys;
    keys.reserve(1000);

    for (int i = 0; i < 1000; ++i) {
        storage.push_back("key_" + std::to_string(i));
        keys.push_back(lsm::Slice(storage.back()));
    }
    b.AddBatch(keys);
    std::string data = b.Finish();

    lsm::BloomFilter filter(data);

    int false_neg = 0;
    for (auto& s : storage)
        if (!filter.MightContain(lsm::Slice(s))) ++false_neg;
    CHECK(false_neg == 0);          // 无假阴性

    int fp = 0;
    for (int i = 0; i < 1000; ++i) {
        std::string m = "missing_" + std::to_string(i);
        if (filter.MightContain(lsm::Slice(m))) ++fp;
    }
    std::printf("  Bloom false positives: %d/1000 (%.1f%%)\n", fp, fp / 10.0);
    CHECK(fp < 50);
}

static void TestAddVsAddBatch() {
    std::vector<std::string> storage;
    for (int i = 0; i < 200; ++i) storage.push_back("k" + std::to_string(i));

    lsm::BloomBuilder b1(10);
    for (auto& s : storage) b1.Add(lsm::Slice(s));

    lsm::BloomBuilder b2(10);
    std::vector<lsm::Slice> slices;
    for (auto& s : storage) slices.push_back(lsm::Slice(s));
    b2.AddBatch(slices);

    CHECK(b1.Finish() == b2.Finish());
}

static void TestConcurrentHash() {
    constexpr int T = 8, ITER = 5000;
    std::string data(128, 'x');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7);

    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAuto);
    uint64_t expected = lsm::BloomHash::HashOne(data.data(), data.size());

    std::atomic<int> mismatches{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < T; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < ITER; ++i) {
                uint64_t v = lsm::BloomHash::HashOne(data.data(), data.size());
                if (v != expected) mismatches.fetch_add(1);
            }
        });
    for (auto& th : ts) th.join();
    CHECK(mismatches.load() == 0);
}

static void TestBackendSwitch() {
    const char* msg = "backend switch test";
    size_t len = std::strlen(msg);

    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kSoftware);
    CHECK(lsm::BloomHash::EffectiveBackendName() == "software-scalar");
    uint64_t sw = lsm::BloomHash::HashOne(msg, len);

    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAuto);
    CHECK(sw == lsm::BloomHash::HashOne(msg, len));

    if (lsm::BloomHash::AVX2Available()) {
        lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAVX2);
        CHECK(lsm::BloomHash::EffectiveBackendName() == "avx2-4way");
        CHECK(sw == lsm::BloomHash::HashOne(msg, len));
    } else {
        lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAVX2);
        CHECK(lsm::BloomHash::EffectiveBackendName() == "software-scalar");
    }
    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAuto);
}

int main() {
    std::printf("=== BloomHash AVX2 tests ===\n");
    std::printf("  AVX2 available: %s\n",
                lsm::BloomHash::AVX2Available() ? "yes" : "no");
    std::printf("  Effective backend: %s\n",
                lsm::BloomHash::EffectiveBackendName().c_str());

    std::printf("[1] Scalar/AVX2 consistency\n");  TestScalarAVX2Consistency();
    std::printf("[2] Batch vs single\n");          TestBatchVsSingle();
    std::printf("[3] Bloom filter\n");             TestBloomFilter();
    std::printf("[4] Add vs AddBatch\n");          TestAddVsAddBatch();
    std::printf("[5] Concurrent hash\n");          TestConcurrentHash();
    std::printf("[6] Backend switch\n");           TestBackendSwitch();

    if (failures == 0) { std::printf("\n[PASS] all tests\n"); return 0; }
    std::printf("\n[FAIL] %d checks failed\n", failures);
    return 1;
}
