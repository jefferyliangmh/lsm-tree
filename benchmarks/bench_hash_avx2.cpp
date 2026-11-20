#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "lsm/hash_avx2.h"

using Clock = std::chrono::steady_clock;
static double SecsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int main() {
    std::printf("=== BloomHash AVX2 benchmark ===\n");
    std::printf("AVX2 available: %s\n\n",
                lsm::BloomHash::AVX2Available() ? "yes" : "no");

    constexpr size_t N = 1000000;
    constexpr size_t KEYLEN = 32;

    std::vector<std::string> storage(N);
    std::mt19937_64 rng(42);
    for (auto& s : storage) {
        s.resize(KEYLEN);
        for (size_t j = 0; j < KEYLEN; ++j)
            s[j] = static_cast<char>(rng() & 0xFF);
    }
    std::vector<const char*> ptrs(N);
    std::vector<size_t> lens(N, KEYLEN);
    for (size_t i = 0; i < N; ++i) ptrs[i] = storage[i].data();

    std::vector<uint64_t> hashes(N);

    auto bench = [&](lsm::BloomHash::Backend b, const char* name) {
        lsm::BloomHash::Configure(b);
        lsm::BloomHash::HashBatch(ptrs.data(), lens.data(), N, hashes.data());

        constexpr int REPS = 10;
        auto t = Clock::now();
        for (int r = 0; r < REPS; ++r)
            lsm::BloomHash::HashBatch(ptrs.data(), lens.data(), N, hashes.data());
        double s = SecsSince(t);
        double total = static_cast<double>(N) * REPS * KEYLEN;
        std::printf("[%-20s] %.3f s  %.2f GB/s  %.2f Mkey/s\n",
                    name, s, total / s / 1e9,
                    static_cast<double>(N) * REPS / s / 1e6);
    };

    bench(lsm::BloomHash::Backend::kSoftware, "software-scalar");
    if (lsm::BloomHash::AVX2Available())
        bench(lsm::BloomHash::Backend::kAVX2, "avx2-4way");

    lsm::BloomHash::Configure(lsm::BloomHash::Backend::kAuto);
    return 0;
}
