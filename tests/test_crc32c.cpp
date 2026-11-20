#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/crc32c.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// 独立参考实现（用于交叉验证）
static uint32_t RefCRC32C(uint32_t init, const char* data, size_t n) {
    static uint32_t table[256];
    static bool init_table = false;
    if (!init_table) {
        constexpr uint32_t poly = 0x82F63B78u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init_table = true;
    }
    uint32_t crc = init ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void TestKnownVectors() {
    CHECK(lsm::CRC32C::Compute("", 0) == 0x00000000u);
    CHECK(lsm::CRC32C::Compute("123456789", 9) == 0xE3069283u);
    const char zero = 0;
    CHECK(lsm::CRC32C::Compute(&zero, 1) == 0x527D5351u);
}

static void TestSoftwareHardwareConsistency() {
    if (!lsm::CRC32C::HardwareAvailable()) {
        std::printf("  (hardware not available, skipping consistency check)\n");
        return;
    }
    std::mt19937_64 rng(0xdeadbeef);
    std::vector<size_t> sizes = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                                 63, 64, 65, 100, 255, 256, 257, 1023, 1024, 4095, 4096};
    for (size_t n : sizes) {
        std::string data(n, '\0');
        for (size_t i = 0; i < n; ++i) data[i] = static_cast<char>(rng() & 0xFF);

        lsm::CRC32C::Configure(lsm::CRC32C::Backend::kSoftware);
        uint32_t sw = lsm::CRC32C::Compute(data.data(), n);

        lsm::CRC32C::Configure(lsm::CRC32C::Backend::kHardware);
        uint32_t hw = lsm::CRC32C::Compute(data.data(), n);

        CHECK(sw == hw);
        CHECK(sw == RefCRC32C(0, data.data(), n));
    }
    lsm::CRC32C::Configure(lsm::CRC32C::Backend::kAuto);
}

static void TestExtendIncremental() {
    std::mt19937_64 rng(42);
    std::vector<size_t> parts = {8, 16, 7, 100, 1024, 3, 64};
    std::string full;
    for (size_t s : parts) {
        std::string p(s, '\0');
        for (size_t i = 0; i < s; ++i) p[i] = static_cast<char>(rng() & 0xFF);
        full += p;
    }

    uint32_t one = lsm::CRC32C::Compute(full.data(), full.size());
    uint32_t inc = 0;
    size_t off = 0;
    for (size_t s : parts) {
        inc = lsm::CRC32C::Extend(inc, full.data() + off, s);
        off += s;
    }
    CHECK(one == inc);
    CHECK(one == RefCRC32C(0, full.data(), full.size()));
}

static void TestBackendSwitch() {
    const char* msg = "backend switch test";
    size_t len = std::strlen(msg);

    lsm::CRC32C::Configure(lsm::CRC32C::Backend::kSoftware);
    CHECK(lsm::CRC32C::EffectiveBackendName() == "software");
    uint32_t sw = lsm::CRC32C::Compute(msg, len);

    lsm::CRC32C::Configure(lsm::CRC32C::Backend::kAuto);
    CHECK(sw == lsm::CRC32C::Compute(msg, len));

    if (lsm::CRC32C::HardwareAvailable()) {
        lsm::CRC32C::Configure(lsm::CRC32C::Backend::kHardware);
        CHECK(lsm::CRC32C::EffectiveBackendName() == "hardware-sse4.2");
        CHECK(sw == lsm::CRC32C::Compute(msg, len));
    } else {
        lsm::CRC32C::Configure(lsm::CRC32C::Backend::kHardware);
        CHECK(lsm::CRC32C::EffectiveBackendName() == "software");
    }
    lsm::CRC32C::Configure(lsm::CRC32C::Backend::kAuto);
}

static void TestConcurrentCompute() {
    constexpr int T = 8, ITER = 5000;
    std::string data(4096, '\0');
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>(i * 31 + 7);

    uint32_t expected = RefCRC32C(0, data.data(), data.size());
    std::atomic<int> mismatches{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < T; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < ITER; ++i) {
                uint32_t v = lsm::CRC32C::Compute(data.data(), data.size());
                if (v != expected) mismatches.fetch_add(1);
            }
        });
    for (auto& th : ts) th.join();
    CHECK(mismatches.load() == 0);
}

static void TestLargeBuffer() {
    constexpr size_t SZ = 1u * 1024 * 1024;
    std::string big(SZ, '\0');
    std::mt19937_64 rng(7);
    for (size_t i = 0; i < SZ; i += 8) {
        uint64_t v = rng();
        std::memcpy(big.data() + i, &v, 8);
    }

    uint32_t expected = RefCRC32C(0, big.data(), SZ);
    CHECK(lsm::CRC32C::Compute(big.data(), SZ) == expected);

    uint32_t crc = 0;
    constexpr size_t CHUNK = 13 * 1024 + 7;
    size_t off = 0;
    while (off < SZ) {
        size_t n = std::min(CHUNK, SZ - off);
        crc = lsm::CRC32C::Extend(crc, big.data() + off, n);
        off += n;
    }
    CHECK(crc == expected);
}

int main() {
    std::printf("=== CRC32C SSE4.2 tests ===\n");
    std::printf("  Hardware available: %s\n",
                lsm::CRC32C::HardwareAvailable() ? "yes" : "no");
    std::printf("  Effective backend:  %s\n",
                lsm::CRC32C::EffectiveBackendName().c_str());

    std::printf("[1] Known vectors\n");          TestKnownVectors();
    std::printf("[2] SW/HW consistency\n");      TestSoftwareHardwareConsistency();
    std::printf("[3] Extend incremental\n");     TestExtendIncremental();
    std::printf("[4] Backend switch\n");         TestBackendSwitch();
    std::printf("[5] Concurrent compute\n");     TestConcurrentCompute();
    std::printf("[6] Large buffer\n");           TestLargeBuffer();

    if (failures == 0) { std::printf("\n[PASS] all tests\n"); return 0; }
    std::printf("\n[FAIL] %d checks failed\n", failures);
    return 1;
}
