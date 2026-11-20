#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// ============================================================================
// 平台与编译器检测
// ============================================================================
#if defined(__x86_64__) || defined(_M_X64)
#define LSM_HASH_X86_64 1
#else
#define LSM_HASH_X86_64 0
#endif

#if defined(_MSC_VER)
    // MSVC 不启用 AVX2 路径（target 属性 + __builtin_cpu_supports 不可用）
#define LSM_HASH_HAS_AVX2_CODE 0
#elif LSM_HASH_X86_64 && !defined(LSM_DISABLE_AVX2_HASH) && \
      (defined(__GNUC__) || defined(__clang__))
#define LSM_HASH_HAS_AVX2_CODE 1
#include <immintrin.h>
#else
#define LSM_HASH_HAS_AVX2_CODE 0
#endif

namespace lsm {

    class BloomHash {
    public:
        enum class Backend : int { kAuto = 0, kSoftware = 1, kAVX2 = 2 };

        static void Configure(Backend b) noexcept {
            backend_.store(static_cast<int>(b), std::memory_order_release);
            impl_.store(static_cast<BatchFn>(nullptr), std::memory_order_release);
            resolved_.store(0, std::memory_order_release);
        }

        static Backend CurrentBackend() noexcept {
            return static_cast<Backend>(backend_.load(std::memory_order_acquire));
        }

        static bool AVX2Available() noexcept {
#if LSM_HASH_HAS_AVX2_CODE
            static const bool has = [] {
                __builtin_cpu_init();
                return __builtin_cpu_supports("avx2");
                }();
            return has;
#else
            return false;
#endif
        }

        static std::string EffectiveBackendName() {
            Resolve();
            return resolved_.load(std::memory_order_acquire) == 2
                ? "avx2-4way" : "software-scalar";
        }

        static void HashBatch(const char* const* keys, const size_t* key_lens,
            size_t count, uint64_t* hashes) noexcept {
            BatchFn fn = Resolve();
            fn(keys, key_lens, count, hashes);
        }

        static uint64_t HashOne(const char* data, size_t n) noexcept {
            const char* k[1] = { data };
            size_t l[1] = { n };
            uint64_t h[1];
            HashBatch(k, l, 1, h);
            return h[0];
        }

    private:
        using BatchFn = void(*)(const char* const*, const size_t*, size_t, uint64_t*);

        static BatchFn Resolve() noexcept {
            BatchFn f = impl_.load(std::memory_order_acquire);
            if (f) return f;
            return SlowResolve();
        }

        static BatchFn SlowResolve() noexcept;

        static void HashBatchSoftware(const char* const* keys, const size_t* key_lens,
            size_t count, uint64_t* hashes) noexcept;

#if LSM_HASH_HAS_AVX2_CODE
        __attribute__((target("avx2")))
            static void HashBatchAVX2(const char* const* keys, const size_t* key_lens,
                size_t count, uint64_t* hashes) noexcept;
#endif

        static inline std::atomic<int>     backend_{ static_cast<int>(Backend::kAuto) };
        static inline std::atomic<BatchFn> impl_{ static_cast<BatchFn>(nullptr) };
        static inline std::atomic<int>     resolved_{ 0 };
    };

    // ============================================================================
    // XXH64 标量实现
    // ============================================================================
    inline uint64_t XXH64_Scalar(const void* input, size_t len, uint64_t seed = 0) {
        constexpr uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
        constexpr uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
        constexpr uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
        constexpr uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
        constexpr uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

        const uint8_t* p = static_cast<const uint8_t*>(input);
        const uint8_t* const end = p + len;
        uint64_t h64;

        auto read32 = [](const uint8_t* ptr) -> uint32_t {
            uint32_t v; std::memcpy(&v, ptr, 4); return v;
            };
        auto read64 = [](const uint8_t* ptr) -> uint64_t {
            uint64_t v; std::memcpy(&v, ptr, 8); return v;
            };
        auto rotl = [](uint64_t x, int r) -> uint64_t {
            return (x << r) | (x >> (64 - r));
            };
        auto round = [&](uint64_t acc, uint64_t input) -> uint64_t {
            acc += input * PRIME64_2;
            acc = rotl(acc, 31);
            acc *= PRIME64_1;
            return acc;
            };
        auto mergeRound = [&](uint64_t acc, uint64_t val) -> uint64_t {
            val = round(0, val);
            acc ^= val;
            acc = acc * PRIME64_1 + PRIME64_4;
            return acc;
            };

        if (len >= 32) {
            uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
            uint64_t v2 = seed + PRIME64_2;
            uint64_t v3 = seed + 0;
            uint64_t v4 = seed - PRIME64_1;

            const uint8_t* const limit = end - 32;
            do {
                v1 = round(v1, read64(p)); p += 8;
                v2 = round(v2, read64(p)); p += 8;
                v3 = round(v3, read64(p)); p += 8;
                v4 = round(v4, read64(p)); p += 8;
            } while (p <= limit);

            h64 = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            h64 = mergeRound(h64, v1);
            h64 = mergeRound(h64, v2);
            h64 = mergeRound(h64, v3);
            h64 = mergeRound(h64, v4);
        }
        else {
            h64 = seed + PRIME64_5;
        }

        h64 += static_cast<uint64_t>(len);

        while (p + 8 <= end) {
            uint64_t k1 = round(0, read64(p));
            h64 ^= k1;
            h64 = rotl(h64, 27) * PRIME64_1 + PRIME64_4;
            p += 8;
        }
        if (p + 4 <= end) {
            h64 ^= static_cast<uint64_t>(read32(p)) * PRIME64_1;
            h64 = rotl(h64, 23) * PRIME64_2 + PRIME64_3;
            p += 4;
        }
        while (p < end) {
            h64 ^= static_cast<uint64_t>(*p) * PRIME64_5;
            h64 = rotl(h64, 11) * PRIME64_1;
            ++p;
        }

        h64 ^= h64 >> 33;
        h64 *= PRIME64_2;
        h64 ^= h64 >> 29;
        h64 *= PRIME64_3;
        h64 ^= h64 >> 32;
        return h64;
    }

    inline uint64_t XXH64_Scalar_NoFinalMix(const void* input, size_t len) {
        constexpr uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
        constexpr uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
        constexpr uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
        constexpr uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
        constexpr uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

        const uint8_t* p = static_cast<const uint8_t*>(input);
        const uint8_t* const end = p + len;
        uint64_t h64;

        auto read32 = [](const uint8_t* ptr) -> uint32_t {
            uint32_t v; std::memcpy(&v, ptr, 4); return v;
            };
        auto read64 = [](const uint8_t* ptr) -> uint64_t {
            uint64_t v; std::memcpy(&v, ptr, 8); return v;
            };
        auto rotl = [](uint64_t x, int r) -> uint64_t {
            return (x << r) | (x >> (64 - r));
            };
        auto round = [&](uint64_t acc, uint64_t input) -> uint64_t {
            acc += input * PRIME64_2;
            acc = rotl(acc, 31);
            acc *= PRIME64_1;
            return acc;
            };
        auto mergeRound = [&](uint64_t acc, uint64_t val) -> uint64_t {
            val = round(0, val);
            acc ^= val;
            acc = acc * PRIME64_1 + PRIME64_4;
            return acc;
            };

        if (len >= 32) {
            uint64_t v1 = PRIME64_1 + PRIME64_2;
            uint64_t v2 = PRIME64_2;
            uint64_t v3 = 0;
            uint64_t v4 = 0 - PRIME64_1;

            const uint8_t* const limit = end - 32;
            do {
                v1 = round(v1, read64(p)); p += 8;
                v2 = round(v2, read64(p)); p += 8;
                v3 = round(v3, read64(p)); p += 8;
                v4 = round(v4, read64(p)); p += 8;
            } while (p <= limit);

            h64 = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            h64 = mergeRound(h64, v1);
            h64 = mergeRound(h64, v2);
            h64 = mergeRound(h64, v3);
            h64 = mergeRound(h64, v4);
        }
        else {
            h64 = PRIME64_5;
        }

        h64 += static_cast<uint64_t>(len);

        while (p + 8 <= end) {
            uint64_t k1 = round(0, read64(p));
            h64 ^= k1;
            h64 = rotl(h64, 27) * PRIME64_1 + PRIME64_4;
            p += 8;
        }
        if (p + 4 <= end) {
            h64 ^= static_cast<uint64_t>(read32(p)) * PRIME64_1;
            h64 = rotl(h64, 23) * PRIME64_2 + PRIME64_3;
            p += 4;
        }
        while (p < end) {
            h64 ^= static_cast<uint64_t>(*p) * PRIME64_5;
            h64 = rotl(h64, 11) * PRIME64_1;
            ++p;
        }
        return h64;
    }

    inline void BloomHash::HashBatchSoftware(const char* const* keys, const size_t* key_lens,
        size_t count, uint64_t* hashes) noexcept {
        for (size_t i = 0; i < count; ++i) {
            hashes[i] = XXH64_Scalar(keys[i], key_lens[i], 0);
        }
    }

#if LSM_HASH_HAS_AVX2_CODE

    // ============================================================================
    // ★ AVX2 64 位乘法模拟
    //
    // _mm256_mullo_epi64（即 vpmullq）是 AVX512VL+AVX512DQ 指令，
    // 在 AVX2-only CPU 上 GCC 11 会报 "target specific option mismatch"。
    //
    // 用 AVX2 原生的 32×32 无符号乘法组合出 64×64 低 64 位结果：
    //   lo64(a*b) = alo*blo + ((alo*bhi + ahi*blo) << 32)
    //   其中 alo = a & 0xFFFFFFFF, ahi = a >> 32
    //
    // 与标量结果完全一致（只取低 64 位），哈希正确性由 lsm_hash_test 验证。
    // ============================================================================
    __attribute__((target("avx2")))
        static inline __m256i LSM_MulloEpi64AVX2(__m256i a, __m256i b) {
        const __m256i mask = _mm256_set1_epi64x(0xFFFFFFFFLL);
        __m256i alo = _mm256_and_si256(a, mask);
        __m256i blo = _mm256_and_si256(b, mask);
        __m256i ahi = _mm256_srli_epi64(a, 32);
        __m256i bhi = _mm256_srli_epi64(b, 32);

        __m256i p_lo = _mm256_mul_epu32(alo, blo);    // alo * blo
        __m256i p_a_b = _mm256_mul_epu32(alo, bhi);    // alo * bhi
        __m256i p_b_a = _mm256_mul_epu32(ahi, blo);    // ahi * blo
        __m256i mid = _mm256_add_epi64(p_a_b, p_b_a);
        __m256i mid_shl = _mm256_slli_epi64(mid, 32);

        return _mm256_add_epi64(p_lo, mid_shl);
    }

    __attribute__((target("avx2")))
        inline void BloomHash::HashBatchAVX2(const char* const* keys, const size_t* key_lens,
            size_t count, uint64_t* hashes) noexcept {
        constexpr uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
        constexpr uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;

        const __m256i prime2 = _mm256_set1_epi64x(static_cast<long long>(PRIME64_2));
        const __m256i prime3 = _mm256_set1_epi64x(static_cast<long long>(PRIME64_3));

        size_t i = 0;
        for (; i + 4 <= count; i += 4) {
            uint64_t h[4];
            h[0] = XXH64_Scalar_NoFinalMix(keys[i + 0], key_lens[i + 0]);
            h[1] = XXH64_Scalar_NoFinalMix(keys[i + 1], key_lens[i + 1]);
            h[2] = XXH64_Scalar_NoFinalMix(keys[i + 2], key_lens[i + 2]);
            h[3] = XXH64_Scalar_NoFinalMix(keys[i + 3], key_lens[i + 3]);

            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h));
            v = _mm256_xor_si256(v, _mm256_srli_epi64(v, 33));
            v = LSM_MulloEpi64AVX2(v, prime2);            // ★ 用模拟函数
            v = _mm256_xor_si256(v, _mm256_srli_epi64(v, 29));
            v = LSM_MulloEpi64AVX2(v, prime3);            // ★ 用模拟函数
            v = _mm256_xor_si256(v, _mm256_srli_epi64(v, 32));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(hashes + i), v);
        }

        for (; i < count; ++i) {
            hashes[i] = XXH64_Scalar(keys[i], key_lens[i], 0);
        }
    }

#endif // LSM_HASH_HAS_AVX2_CODE

    inline BloomHash::BatchFn BloomHash::SlowResolve() noexcept {
        BatchFn already = impl_.load(std::memory_order_acquire);
        if (already) return already;

        Backend b = CurrentBackend();

        if (b == Backend::kAuto) {
            if (const char* env = std::getenv("LSM_HASH_BACKEND")) {
                if (std::strcmp(env, "software") == 0)  b = Backend::kSoftware;
                else if (std::strcmp(env, "avx2") == 0) b = Backend::kAVX2;
            }
        }

        BatchFn f = nullptr;
        int resolved = 1;

        if (b == Backend::kAVX2 || b == Backend::kAuto) {
#if LSM_HASH_HAS_AVX2_CODE
            if (AVX2Available()) {
                f = &HashBatchAVX2;
                resolved = 2;
            }
#endif
        }
        if (!f) {
            f = &HashBatchSoftware;
            resolved = 1;
        }

        impl_.store(f, std::memory_order_release);
        resolved_.store(resolved, std::memory_order_release);
        return f;
    }

} // namespace lsm