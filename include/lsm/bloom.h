#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "lsm/slice.h"
#include "lsm/hash_avx2.h"

namespace lsm {

#if defined(__GNUC__) || defined(__clang__)
#define LSM_PREFETCH_T0(addr) __builtin_prefetch((addr), 0, 3)
#else
#define LSM_PREFETCH_T0(addr) ((void)0)
#endif

    inline uint32_t Hash32(const char* data, size_t n, uint32_t seed = 0xbc9f1d34u) {
        uint32_t h = seed;
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<uint8_t>(data[i]);
            h *= 0x5bd1e995u;
            h ^= h >> 15;
        }
        return h;
    }

    class BloomBuilder {
    public:
        explicit BloomBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {
            k_ = static_cast<int>(bits_per_key * 0.69);
            if (k_ < 1)  k_ = 1;
            if (k_ > 30) k_ = 30;
        }

        // 单条添加：走 HashOne（内部 count=1 标量路径）
        void Add(const Slice& key) {
            uint64_t h64 = BloomHash::HashOne(key.data(), key.size());
            hashes_.push_back(h64);
        }

        // ★ 批量添加：一次触发 AVX2 4 路并行哈希
        void AddBatch(const std::vector<Slice>& keys) {
            size_t n = keys.size();
            if (n == 0) return;

            std::vector<const char*> ptrs(n);
            std::vector<size_t> lens(n);
            for (size_t i = 0; i < n; ++i) {
                ptrs[i] = keys[i].data();
                lens[i] = keys[i].size();
            }
            std::vector<uint64_t> h64s(n);
            BloomHash::HashBatch(ptrs.data(), lens.data(), n, h64s.data());

            hashes_.reserve(hashes_.size() + n);
            for (size_t i = 0; i < n; ++i) hashes_.push_back(h64s[i]);
        }

        // 直接添加已计算的 64 位哈希（TableBuilder 用）
        void AddRaw(uint64_t h64) {
            hashes_.push_back(h64);
        }

        std::string Finish() const {
            if (hashes_.empty()) return {};
            size_t bits = hashes_.size() * static_cast<size_t>(bits_per_key_);
            if (bits < 64) bits = 64;
            size_t bytes = (bits + 7) / 8;
            bits = bytes * 8;

            std::string out(bytes, '\0');
            for (uint64_t h64 : hashes_) {
                uint32_t h1 = static_cast<uint32_t>(h64);
                uint32_t h2 = static_cast<uint32_t>(h64 >> 32);
                uint32_t delta = (h2 >> 17) | (h2 << 15);
                uint32_t x = h1;
                for (int i = 0; i < k_; ++i) {
                    size_t pos = x % bits;
                    out[pos / 8] = static_cast<char>(out[pos / 8] | (1 << (pos % 8)));
                    x += delta;
                }
            }
            out.push_back(static_cast<char>(k_));
            return out;
        }

        size_t NumKeys() const noexcept { return hashes_.size(); }

    private:
        int bits_per_key_;
        int k_;
        std::vector<uint64_t> hashes_;
    };

    // ============================================================================
    // BloomFilter：FastMod + 预取流水线
    //
    // 关键优化：
    //   1. FastMod：用乘法逆元替代 % bits（30 cycle → 3 cycle）
    //   2. 批量算位置 → 一次性预取 → 标量检查（保持提前退出）
    //   3. 不做 AVX2 gather（实测比标量慢，且无法提前退出）
    //
    // 数学保证（bits < 2^31 时）：
    //   reciprocal = 2^32 / bits
    //   q = (x * reciprocal) >> 32，误差 <= 1
    //   r = x - q * bits，最多修正一次
    //   实际 Bloom 大小 ~178KB × 8 = 1.4M bit，远小于 2^31，安全
    // ============================================================================
    class BloomFilter {
    public:
        BloomFilter() = default;

        explicit BloomFilter(std::string data) : data_(std::move(data)) {
            if (data_.empty()) return;
            int k = static_cast<uint8_t>(data_.back());
            if (k > 30) return;
            k_ = k;
            uint64_t bits64 = (data_.size() - 1) * 8;
            if (bits64 == 0 || bits64 > 0x7FFFFFFFull) return;
            bits_ = static_cast<uint32_t>(bits64);
            bits_reciprocal_ = static_cast<uint32_t>((1ULL << 32) / bits_);
        }

        bool MightContain(const Slice& key) const {
            if (data_.empty() || k_ == 0 || bits_ == 0) return true;

            uint64_t h64 = BloomHash::HashOne(key.data(), key.size());
            uint32_t h1 = static_cast<uint32_t>(h64);
            uint32_t h2 = static_cast<uint32_t>(h64 >> 32);
            uint32_t delta = (h2 >> 17) | (h2 << 15);

            const char* base = data_.data();
            const uint32_t bits = bits_;
            const uint32_t recip = bits_reciprocal_;

            // 批量算位置（无除法）
            uint32_t pos[30];
            uint32_t x = h1;
            for (int i = 0; i < k_; ++i) {
                pos[i] = FastMod(x, bits, recip);
                x += delta;
            }

            // 一次性预取
            for (int i = 0; i < k_; ++i) {
                LSM_PREFETCH_T0(base + pos[i] / 8);
            }

            // 标量检查，支持提前退出
            for (int i = 0; i < k_; ++i) {
                if (!(base[pos[i] / 8] & (1 << (pos[i] % 8)))) return false;
            }
            return true;
        }

        bool Empty() const noexcept { return data_.empty(); }

    private:
        static inline uint32_t FastMod(uint32_t x, uint32_t bits, uint32_t recip) {
            uint32_t q = static_cast<uint32_t>(
                (static_cast<uint64_t>(x) * recip) >> 32);
            uint32_t r = x - q * bits;
            if (r >= bits) r -= bits;
            return r;
        }

        std::string data_;
        uint32_t    bits_ = 0;
        uint32_t    bits_reciprocal_ = 0;
        int         k_ = 0;
    };

} // namespace lsm