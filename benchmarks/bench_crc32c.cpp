#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "lsm/crc32c.h"

using Clock = std::chrono::steady_clock;
static double SecsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int main() {
    std::printf("=== CRC32C benchmark ===\n");
    std::printf("Hardware available: %s\n\n",
                lsm::CRC32C::HardwareAvailable() ? "yes" : "no");

    constexpr size_t SZ = 16ull * 1024 * 1024;
    constexpr int ITERS = 20;
    std::string data(SZ, '\0');
    std::mt19937_64 rng(42);
    for (size_t i = 0; i < SZ; i += 8) {
        uint64_t v = rng();
        std::memcpy(data.data() + i, &v, 8);
    }
    volatile uint32_t sink = 0;

    lsm::CRC32C::Configure(lsm::CRC32C::Backend::kSoftware);
    for (int i = 0; i < 2; ++i) sink ^= lsm::CRC32C::Compute(data.data(), SZ);
    auto t0 = Clock::now();
    for (int i = 0; i < ITERS; ++i) sink ^= lsm::CRC32C::Compute(data.data(), SZ);
    double sw_s = SecsSince(t0);
    std::printf("[software] %.3f s  %.2f GB/s\n", sw_s, (double)SZ * ITERS / sw_s / 1e9);

    if (lsm::CRC32C::HardwareAvailable()) {
        lsm::CRC32C::Configure(lsm::CRC32C::Backend::kHardware);
        for (int i = 0; i < 2; ++i) sink ^= lsm::CRC32C::Compute(data.data(), SZ);
        t0 = Clock::now();
        for (int i = 0; i < ITERS; ++i) sink ^= lsm::CRC32C::Compute(data.data(), SZ);
        double hw_s = SecsSince(t0);
        double hw_gbs = (double)SZ * ITERS / hw_s / 1e9;
        std::printf("[hardware] %.3f s  %.2f GB/s  (speedup %.2fx)\n",
                    hw_s, hw_gbs, sw_s / hw_s);
    }

    std::printf("\n[WAL scenario: 200-byte records]\n");
    std::vector<std::string> recs;
    for (int i = 0; i < 1000; ++i) {
        std::string r(200, '\0');
        for (auto& ch : r) ch = static_cast<char>(rng() & 0xFF);
        recs.push_back(std::move(r));
    }

    auto bench = [&](lsm::CRC32C::Backend b, const char* name) {
        lsm::CRC32C::Configure(b);
        for (int w = 0; w < 3; ++w)
            for (auto& r : recs) sink ^= lsm::CRC32C::Compute(r.data(), r.size());

        constexpr int REPS = 500;
        auto t = Clock::now();
        for (int k = 0; k < REPS; ++k)
            for (auto& r : recs) sink ^= lsm::CRC32C::Compute(r.data(), r.size());
        double s = SecsSince(t);
        double ops = (double)recs.size() * REPS;
        std::printf("  [%s] %.3f s  %.2f GB/s  %.2f Mops/s\n",
                    name, s, ops * 200 / s / 1e9, ops / s / 1e6);
    };
    bench(lsm::CRC32C::Backend::kSoftware, "software");
    if (lsm::CRC32C::HardwareAvailable())
        bench(lsm::CRC32C::Backend::kHardware, "hardware");

    lsm::CRC32C::Configure(lsm::CRC32C::Backend::kAuto);
    (void)sink;
    return 0;
}
