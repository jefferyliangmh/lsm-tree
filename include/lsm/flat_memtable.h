#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "lsm/internal_key.h"

namespace lsm {

    // ============================================================================
    // AppendOnlySimdTable
    //
    // 替代 skiplist 的 memtable：
    //   - 记录只追加进稳定 chunk（地址发布后不再移动）。
    //   - 索引按 user key 分成 kShards 个有序数组。一次点查只做一次无分支二分，
    //     而不是扫过每一个 segment（随机写入时 segment 范围互相重叠）。
    //   - user key >= 16B 走 SSE2；在 AVX2 机器上 >=32B 前缀走 AVX2。
    //
    // 并发：写者由 DB::memtable_mu_ 独占。读者通过 atomic<shared_ptr<Snapshot>>
    // 拿到不可变索引；未改动的分片 shared_ptr 共享，chunk 字节一旦发布就不再改写。
    // ============================================================================
    class AppendOnlySimdTable {
        struct Chunk {
            explicit Chunk(size_t cap) : data(new char[cap]), cap(cap) {}
            std::unique_ptr<char[]> data;
            size_t cap = 0;
            size_t used = 0;
        };

        static Slice KeyOf(const char* rec) noexcept {
            uint32_t klen = 0;
            std::memcpy(&klen, rec, 4);
            return Slice(rec + 8, klen);
        }

        static Slice ValueOf(const char* rec) noexcept {
            uint32_t klen = 0, vlen = 0;
            std::memcpy(&klen, rec, 4);
            std::memcpy(&vlen, rec + 4, 4);
            return Slice(rec + 8 + klen, vlen);
        }

        static void WriteRecord(char* dest, const std::string& key, const std::string& value) {
            const uint32_t klen = static_cast<uint32_t>(key.size());
            const uint32_t vlen = static_cast<uint32_t>(value.size());
            std::memcpy(dest, &klen, 4);
            std::memcpy(dest + 4, &vlen, 4);
            if (klen) std::memcpy(dest + 8, key.data(), klen);
            if (vlen) std::memcpy(dest + 8 + klen, value.data(), vlen);
        }

        static int CompareInternalFast(const Slice& a, const Slice& b) noexcept;

        std::vector<std::unique_ptr<Chunk>> chunks_;
        std::atomic<size_t> size_{ 0 };
        std::atomic<size_t> bytes_{ 0 };
        std::atomic<size_t> memory_bytes_{ 0 };

        const char* AppendRecord(const std::string& key, const std::string& value) {
            const size_t need = 8 + key.size() + value.size();
            if (!chunks_.empty()) {
                Chunk& tail = *chunks_.back();
                if (tail.used + need <= tail.cap) {
                    char* dest = tail.data.get() + tail.used;
                    WriteRecord(dest, key, value);
                    tail.used += need;
                    return dest;
                }
            }
            const size_t cap = std::max(need, static_cast<size_t>(4096));
            chunks_.push_back(std::make_unique<Chunk>(cap));
            memory_bytes_.fetch_add(cap, std::memory_order_relaxed);
            Chunk& created = *chunks_.back();
            char* dest = created.data.get();
            WriteRecord(dest, key, value);
            created.used = need;
            return dest;
        }

    public:
        static constexpr size_t kShards = 32;

        struct Snapshot {
            std::array<std::shared_ptr<const std::vector<const char*>>, kShards> shards;
        };

        struct Lookup {
            bool        found = false;
            std::string value;
            ValueType   type = ValueType::kValue;
        };

        AppendOnlySimdTable() {
            published_.store(std::make_shared<const Snapshot>(), std::memory_order_relaxed);
        }

        static uint32_t ShardOf(const std::string& user_key) noexcept {
            uint32_t h = 2166136261u;
            for (unsigned char c : user_key) {
                h ^= c;
                h *= 16777619u;
            }
            return h & static_cast<uint32_t>(kShards - 1);
        }

        void Add(uint64_t seq, const std::string& user_key,
            const std::string& value, ValueType type) {
            const std::string ikey = MakeInternalKey(user_key, seq, type);
            const char* rec = AppendRecord(ikey, value);
            const uint32_t shard = ShardOf(user_key);

            auto current = published_.load(std::memory_order_acquire);
            auto next = std::make_shared<Snapshot>(*current);
            auto keys = std::make_shared<std::vector<const char*>>();
            if (current->shards[shard]) *keys = *current->shards[shard];
            const size_t idx = BranchlessLowerBound(*keys, Slice(ikey));
            keys->insert(keys->begin() + static_cast<std::ptrdiff_t>(idx), rec);
            memory_bytes_.fetch_add(sizeof(const char*), std::memory_order_relaxed);
            next->shards[shard] = std::shared_ptr<const std::vector<const char*>>(std::move(keys));
            published_.store(std::move(next), std::memory_order_release);
            size_.fetch_add(1, std::memory_order_relaxed);
            bytes_.fetch_add(ikey.size() + value.size(), std::memory_order_relaxed);
        }

        Lookup Get(const std::string& user_key, uint64_t snapshot_seq) const {
            Lookup result;
            const std::string target = MakeInternalKey(user_key, snapshot_seq, ValueType::kValue);
            const Slice target_slice(target);
            auto snap = published_.load(std::memory_order_acquire);
            const auto& shard = snap->shards[ShardOf(user_key)];
            if (!shard || shard->empty()) return result;

            const size_t idx = BranchlessLowerBound(*shard, target_slice);
            if (idx >= shard->size()) return result;
            const char* best = (*shard)[idx];
            const Slice key = KeyOf(best);
            if (key.size() < 8) return result;
            const size_t user_len = key.size() - 8;
            if (user_len != user_key.size() ||
                std::memcmp(key.data(), user_key.data(), user_len) != 0)
                return result;
            const uint64_t tag = DecodeFixed64BE(key.data() + user_len);
            if ((tag >> 8) > snapshot_seq) return result;
            result.found = true;
            const Slice val = ValueOf(best);
            result.value.assign(val.data(), val.size());
            result.type = static_cast<ValueType>(tag & 0xFF);
            return result;
        }

        class Iterator {
        public:
            Iterator() = default;
            explicit Iterator(std::shared_ptr<const Snapshot> snap)
                : snap_(std::move(snap)) {
                cursors_.fill(0);
                Step();
            }

            bool Valid() const noexcept { return valid_; }
            Slice Key() const noexcept { return key_; }
            Slice Value() const noexcept { return value_; }

            void Next() {
                if (!valid_) return;
                ++cursors_[static_cast<size_t>(chosen_)];
                Step();
            }

        private:
            void Step() {
                valid_ = false;
                chosen_ = -1;
                if (!snap_) return;
                const char* best = nullptr;
                for (size_t i = 0; i < kShards; ++i) {
                    const auto& shard = snap_->shards[i];
                    if (!shard || cursors_[i] >= shard->size()) continue;
                    const char* rec = (*shard)[cursors_[i]];
                    if (!best || CompareInternalFast(KeyOf(rec), KeyOf(best)) < 0) {
                        best = rec;
                        chosen_ = static_cast<int>(i);
                    }
                }
                if (!best) return;
                key_ = KeyOf(best);
                value_ = ValueOf(best);
                valid_ = true;
            }

            std::shared_ptr<const Snapshot> snap_;
            std::array<size_t, kShards> cursors_{};
            int chosen_ = -1;
            Slice key_{};
            Slice value_{};
            bool valid_ = false;
        };

        Iterator NewIterator() const {
            return Iterator(published_.load(std::memory_order_acquire));
        }

        size_t Size() const noexcept { return size_.load(std::memory_order_relaxed); }
        size_t Bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }
        size_t MemoryUsage() const noexcept {
            return memory_bytes_.load(std::memory_order_relaxed);
        }

        // 已排序记录指针上的无分支 lower_bound。返回第一个 >= target 的下标。
        static size_t BranchlessLowerBound(const std::vector<const char*>& keys,
            const Slice& target) {
            size_t base = 0;
            size_t n = keys.size();
            while (n > 1) {
                const size_t half = n >> 1;
                const size_t mid = base + half;
#if defined(__GNUC__) || defined(__clang__)
                if (half > 1) __builtin_prefetch(keys[base + (half >> 1)], 0, 1);
#endif
                const int cmp = CompareInternalFast(KeyOf(keys[mid]), target);
                base += (cmp < 0) ? half : 0;
                n -= half;
            }
            if (n == 1 && base < keys.size() &&
                CompareInternalFast(KeyOf(keys[base]), target) < 0)
                ++base;
            return base;
        }

    private:
        std::atomic<std::shared_ptr<const Snapshot>> published_;
    };

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(LSM_DISABLE_AVX2_COMPARE)
    __attribute__((target("avx2")))
    inline int CompareUserKeysAvx2(const Slice& ua, const Slice& ub) noexcept;
#endif

    inline int AppendOnlySimdTable::CompareInternalFast(const Slice& a, const Slice& b) noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(LSM_DISABLE_AVX2_COMPARE)
        static const int avx2 = [] {
            __builtin_cpu_init();
            return __builtin_cpu_supports("avx2") ? 1 : 0;
        }();
        if (avx2 && a.size() >= 8 && b.size() >= 8) {
            const int prefix = CompareUserKeysAvx2(UserKeyOf(a), UserKeyOf(b));
            if (prefix != 0) return prefix;
            const uint64_t sa = SeqOf(a);
            const uint64_t sb = SeqOf(b);
            if (sa != sb) return sa > sb ? -1 : 1;
            const auto ta = TypeOf(a);
            const auto tb = TypeOf(b);
            if (ta != tb) return ta > tb ? -1 : 1;
            return 0;
        }
#endif
        return CompareInternal(a, b);
    }

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(LSM_DISABLE_AVX2_COMPARE)
    __attribute__((target("avx2")))
    inline int CompareUserKeysAvx2(const Slice& ua, const Slice& ub) noexcept {
        const size_t n = ua.size() < ub.size() ? ua.size() : ub.size();
        size_t i = 0;
        while (n - i >= 32) {
            const __m256i va = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(ua.data() + i));
            const __m256i vb = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(ub.data() + i));
            const uint32_t mask = static_cast<uint32_t>(
                _mm256_movemask_epi8(_mm256_cmpeq_epi8(va, vb)));
            if (mask != 0xFFFFFFFFu) {
                const int idx = LSM_CountTrailingZeros32(~mask);
                return static_cast<int>(static_cast<uint8_t>(ua.data()[i + idx])) -
                    static_cast<int>(static_cast<uint8_t>(ub.data()[i + idx]));
            }
            i += 32;
        }
        if (i < n) {
            const int r = std::memcmp(ua.data() + i, ub.data() + i, n - i);
            if (r != 0) return r;
        }
        if (ua.size() < ub.size()) return -1;
        if (ua.size() > ub.size()) return 1;
        return 0;
    }
#endif

} // namespace lsm
