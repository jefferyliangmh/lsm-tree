#pragma once
#include <atomic>
#include <cstdint>

namespace lsm {

    // 进程级 I/O 计数。TableReader 不持有 DB 指针，块读在这里累计。
    // 基准程序在每次运行前 Reset。多 DB 并发时这些计数会混在一起。
    struct EngineCounters {
        static std::atomic<uint64_t> block_reads;
        static std::atomic<uint64_t> block_read_bytes;
        static std::atomic<uint64_t> block_cache_hits;
        static std::atomic<uint64_t> prefetch_blocks;
        static std::atomic<uint64_t> prefetch_bytes;

        static void Reset() noexcept {
            block_reads.store(0, std::memory_order_relaxed);
            block_read_bytes.store(0, std::memory_order_relaxed);
            block_cache_hits.store(0, std::memory_order_relaxed);
            prefetch_blocks.store(0, std::memory_order_relaxed);
            prefetch_bytes.store(0, std::memory_order_relaxed);
        }
    };

    // 单库过滤器遥测。假阳性 = Bloom/前缀模型判可能存在，但表内没有该 user key。
    struct FilterTelemetry {
        std::atomic<uint64_t> lookups{ 0 };
        std::atomic<uint64_t> bloom_rejects{ 0 };
        std::atomic<uint64_t> bloom_hits{ 0 };
        std::atomic<uint64_t> false_positives{ 0 };
        std::atomic<uint64_t> found{ 0 };
        std::atomic<int> tuned_bits{ 10 };

        void ResetWindow() noexcept {
            bloom_rejects.store(0, std::memory_order_relaxed);
            bloom_hits.store(0, std::memory_order_relaxed);
            false_positives.store(0, std::memory_order_relaxed);
        }
    };

} // namespace lsm
