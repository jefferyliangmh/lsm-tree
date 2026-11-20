#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
    #define LSM_CRC_X86_64 1
#else
    #define LSM_CRC_X86_64 0
#endif

#if defined(__i386__) || defined(_M_IX86)
    #define LSM_CRC_X86_32 1
#else
    #define LSM_CRC_X86_32 0
#endif

#if (LSM_CRC_X86_64 || LSM_CRC_X86_32) && \
    !defined(LSM_DISABLE_HW_CRC) && \
    (defined(__GNUC__) || defined(__clang__))
    #define LSM_CRC_HAS_SSE42_CODE 1
    #include <cpuid.h>
    #include <nmmintrin.h>
#else
    #define LSM_CRC_HAS_SSE42_CODE 0
#endif

namespace lsm {

class CRC32C {
public:
    enum class Backend : int {
        kAuto     = 0,
        kSoftware = 1,
        kHardware = 2,
    };

    static void Configure(Backend b) noexcept {
        backend_.store(static_cast<int>(b), std::memory_order_release);
        impl_.store(static_cast<ImplFn>(nullptr), std::memory_order_release);
        resolved_.store(0, std::memory_order_release);
    }

    static Backend CurrentBackend() noexcept {
        return static_cast<Backend>(backend_.load(std::memory_order_acquire));
    }

    static bool HardwareAvailable() noexcept {
#if LSM_CRC_HAS_SSE42_CODE
        static const bool has = [] {
            unsigned a, b, c, d;
            if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
            return (c & (1u << 20)) != 0;
        }();
        return has;
#else
        return false;
#endif
    }

    static std::string EffectiveBackendName() {
        Resolve();
        int r = resolved_.load(std::memory_order_acquire);
        return r == 2 ? "hardware-sse4.2" : "software";
    }

    static uint32_t Compute(const char* data, size_t n) noexcept {
        return Extend(0, data, n);
    }

    static uint32_t Extend(uint32_t init_crc, const char* data, size_t n) noexcept {
        ImplFn fn = Resolve();
        return fn(init_crc, data, n);
    }

private:
    using ImplFn = uint32_t(*)(uint32_t, const char*, size_t);

    static ImplFn Resolve() noexcept {
        ImplFn f = impl_.load(std::memory_order_acquire);
        if (f) return f;
        return SlowResolve();
    }

    static ImplFn SlowResolve() noexcept;

    static uint32_t ComputeSoftware(uint32_t init, const char* data, size_t n) noexcept;

#if LSM_CRC_HAS_SSE42_CODE
    __attribute__((target("sse4.2")))
    static uint32_t ComputeHardware(uint32_t init, const char* data, size_t n) noexcept;
#endif

    static inline std::atomic<int>    backend_{static_cast<int>(Backend::kAuto)};
    static inline std::atomic<ImplFn> impl_{static_cast<ImplFn>(nullptr)};
    static inline std::atomic<int>    resolved_{0};
};

inline uint32_t CRC32C::ComputeSoftware(uint32_t init, const char* data, size_t n) noexcept {
    static const uint32_t* table = [] {
        static uint32_t t[256];
        constexpr uint32_t poly = 0x82F63B78u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = init ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

#if LSM_CRC_HAS_SSE42_CODE
__attribute__((target("sse4.2")))
inline uint32_t CRC32C::ComputeHardware(uint32_t init, const char* data, size_t n) noexcept {
    uint32_t crc = init ^ 0xFFFFFFFFu;

#if LSM_CRC_X86_64
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, data, 8);
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v));
        data += 8;
        n -= 8;
    }
#endif

    if (n >= 4) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        crc = _mm_crc32_u32(crc, v);
        data += 4;
        n -= 4;
    }

    while (n > 0) {
        crc = _mm_crc32_u8(crc, static_cast<uint8_t>(*data));
        ++data;
        --n;
    }

    return crc ^ 0xFFFFFFFFu;
}
#endif

inline CRC32C::ImplFn CRC32C::SlowResolve() noexcept {
    ImplFn already = impl_.load(std::memory_order_acquire);
    if (already) return already;

    Backend b = CurrentBackend();

    if (b == Backend::kAuto) {
        if (const char* env = std::getenv("LSM_CRC_BACKEND")) {
            if (std::strcmp(env, "software") == 0)      b = Backend::kSoftware;
            else if (std::strcmp(env, "hardware") == 0) b = Backend::kHardware;
        }
    }

    ImplFn f = nullptr;
    int resolved = 1;

    if (b == Backend::kHardware || b == Backend::kAuto) {
#if LSM_CRC_HAS_SSE42_CODE
        if (HardwareAvailable()) {
            f = &ComputeHardware;
            resolved = 2;
        }
#endif
    }

    if (!f) {
        f = &ComputeSoftware;
        resolved = 1;
    }

    impl_.store(f, std::memory_order_release);
    resolved_.store(resolved, std::memory_order_release);
    return f;
}

} // namespace lsm
