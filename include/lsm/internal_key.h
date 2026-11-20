#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "lsm/slice.h"

// ============================================================================
// 平台与编译器检测
// ============================================================================
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define LSM_IK_HAS_SSE2 1
#else
#define LSM_IK_HAS_SSE2 0
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#include <stdlib.h>
inline int LSM_CountTrailingZeros32(uint32_t x) {
    unsigned long idx;
    _BitScanForward(&idx, x);
    return static_cast<int>(idx);
}
#define LSM_BSWAP64(x) _byteswap_uint64(x)
#else
inline int LSM_CountTrailingZeros32(uint32_t x) {
    return __builtin_ctz(x);
}
#define LSM_BSWAP64(x) __builtin_bswap64(x)
#endif

namespace lsm {

    enum class ValueType : uint8_t { kDeletion = 0, kValue = 1 };

    inline void EncodeFixed64BE(char* buf, uint64_t v) noexcept {
        v = LSM_BSWAP64(v);
        std::memcpy(buf, &v, 8);
    }

    inline uint64_t DecodeFixed64BE(const char* p) noexcept {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return LSM_BSWAP64(v);
    }

    inline std::string MakeInternalKey(const std::string& user_key,
        uint64_t seq, ValueType type) {
        std::string out;
        out.reserve(user_key.size() + 8);
        out.append(user_key);
        char buf[8];
        EncodeFixed64BE(buf, (seq << 8) | static_cast<uint8_t>(type));
        out.append(buf, 8);
        return out;
    }

    inline Slice UserKeyOf(const Slice& ikey) noexcept {
        if (ikey.size() < 8) return Slice{};
        return { ikey.data(), ikey.size() - 8 };
    }

    inline uint64_t SeqOf(const Slice& ikey) noexcept {
        if (ikey.size() < 8) return 0;
        return DecodeFixed64BE(ikey.data() + ikey.size() - 8) >> 8;
    }

    inline ValueType TypeOf(const Slice& ikey) noexcept {
        if (ikey.size() < 8) return ValueType::kDeletion;
        return static_cast<ValueType>(
            DecodeFixed64BE(ikey.data() + ikey.size() - 8) & 0xFF);
    }

    // ============================================================================
    // SIMD 加速的 user_key 比较
    //
    // 对 ≥ 16 字节的 key，用 SSE2 一次比较 16 字节，找到第一个不同字节。
    // 对 < 16 字节的 key，走 memcmp（短 key 下 SSE 开销无收益）。
    // 相同前缀后降级到 memcmp 比较剩余，最后用长度决定相等情况。
    // ============================================================================
    inline int CompareUserKeys(const Slice& ua, const Slice& ub) noexcept {
        size_t n = ua.size() < ub.size() ? ua.size() : ub.size();
#if LSM_IK_HAS_SSE2
        if (n >= 16) {
            __m128i va = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(ua.data()));
            __m128i vb = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(ub.data()));
            uint32_t eq_mask = static_cast<uint32_t>(
                _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)));
            if (eq_mask != 0xFFFF) {
                int idx = LSM_CountTrailingZeros32((~eq_mask) & 0xFFFF);
                return static_cast<int>(static_cast<uint8_t>(ua.data()[idx]))
                    - static_cast<int>(static_cast<uint8_t>(ub.data()[idx]));
            }
            int r = std::memcmp(ua.data() + 16, ub.data() + 16, n - 16);
            if (r != 0) return r;
        }
        else
#endif
        {
            int r = std::memcmp(ua.data(), ub.data(), n);
            if (r != 0) return r;
        }
        if (ua.size() < ub.size()) return -1;
        if (ua.size() > ub.size()) return 1;
        return 0;
    }

    // ============================================================================
    // 内部键比较：user_key 升序 → seq 降序 → type 降序
    // ============================================================================
    inline int CompareInternal(const Slice& a, const Slice& b) noexcept {
        if (a.size() < 8 || b.size() < 8) {
            return a.compare(b);
        }
        Slice ua = UserKeyOf(a), ub = UserKeyOf(b);
        int r = CompareUserKeys(ua, ub);
        if (r != 0) return r;
        uint64_t sa = SeqOf(a), sb = SeqOf(b);
        if (sa != sb) return sa > sb ? -1 : 1;
        auto ta = TypeOf(a), tb = TypeOf(b);
        if (ta != tb) return ta > tb ? -1 : 1;
        return 0;
    }

} // namespace lsm