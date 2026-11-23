#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace lsm {

    // Leveled：层间有序、无重叠，读放大低、写放大高。
    // SizeTiered：同层允许重叠，只合并本层文件再晋升，写放大低。
    // Hybrid：写密集时对年轻层走 tiered，读密集或深层走 leveled
    // （Amethyst 式的 segment 级策略选择，而不是全局静态 leveled）。
    enum class CompactionStyle : uint8_t {
        kLeveled = 0,
        kSizeTiered = 1,
        kHybrid = 2
    };

    // kSkipList：指针跳跃表，O(log n) 插入。
    // kAppendOnlySimd：arena 追加记录 + 分段排序，点查走无分支二分和 SIMD 键比较。
    enum class MemTableType : uint8_t {
        kSkipList = 0,
        kAppendOnlySimd = 1
    };

    // kStandardBloom：建表时固定 bits-per-key。
    // kAdaptiveBloom：按观测假阳性率在运行期调整后续表的 bits-per-key，并按层倾斜。
    // kLearnedBloom：在 Bloom 前加精确前缀模型（无假阴性），再配合自适应位宽。
    enum class FilterKind : uint8_t {
        kStandardBloom = 0,
        kAdaptiveBloom = 1,
        kLearnedBloom = 2
    };

    struct Options {
        std::string db_path = "./lsm_db";
        size_t      memtable_bytes = 4ull * 1024 * 1024;
        size_t      memtable_entries = 100000;
        int         bloom_bits_per_key = 10;
        int         index_interval = 16;
        int         level0_trigger = 4;
        int         max_levels = 7;
        bool        sync_wal = false;
        bool        use_bloom = true;
        int         bg_interval_ms = 50;
        size_t      block_restart_interval = 16;

        size_t      block_size = 4096;
        size_t      block_cache_capacity = 64ull * 1024 * 1024;
        int         block_cache_shard_bits = 4;

        size_t      max_file_size = 2ull * 1024 * 1024;
        size_t      max_bytes_for_level_base = 64ull * 1024 * 1024;
        double      max_bytes_for_level_multiplier = 10.0;

        size_t      immutable_slowdown_threshold = 2;
        size_t      immutable_stop_threshold = 4;
        uint32_t    write_slowdown_delay_ms = 1;
        bool        collect_write_path_stats = false;

        // --- compaction 策略（默认 leveled，与既有行为一致） ---
        CompactionStyle compaction_style = CompactionStyle::kLeveled;
        int         tiered_fanout = 4;
        // puts/gets >= 该比值时，hybrid 的年轻层改走 tiered。
        double      hybrid_write_heavy_ratio = 4.0;
        // 层号 >= 该值时 hybrid 始终 leveled，保证深层无重叠。
        int         hybrid_leveled_from = 2;
        // 由 DB 在挑选 compaction 前写入，用户无需设置。
        double      observed_write_read_ratio = 0.0;

        // --- 后台调度 ---
        // flush 必须按 immutable 队列顺序执行，因此 flush 池默认 1。
        // compaction 池与 flush 池分离，使归并 I/O 与前台 flush 重叠。
        int         flush_threads = 1;
        int         compaction_threads = 1;
        // compaction 排队（含正在执行）达到该值时，写路径额外减速。
        size_t      max_compaction_backlog = 2;
        // compaction 读阶段用 AsyncIOBackend 预取数据块，再与归并重叠。
        bool        pipelined_compaction_io = true;

        // --- memtable / filter ---
        MemTableType memtable_type = MemTableType::kSkipList;
        FilterKind  filter_kind = FilterKind::kStandardBloom;
        double      bloom_target_fpr = 0.01;
        int         bloom_bits_min = 4;
        int         bloom_bits_max = 16;
    };

    inline const char* CompactionStyleName(CompactionStyle style) noexcept {
        switch (style) {
        case CompactionStyle::kSizeTiered: return "size-tiered";
        case CompactionStyle::kHybrid: return "hybrid";
        case CompactionStyle::kLeveled:
        default: return "leveled";
        }
    }

    inline const char* MemTableTypeName(MemTableType type) noexcept {
        switch (type) {
        case MemTableType::kAppendOnlySimd: return "append-only-simd";
        case MemTableType::kSkipList:
        default: return "skiplist";
        }
    }

    inline const char* FilterKindName(FilterKind kind) noexcept {
        switch (kind) {
        case FilterKind::kAdaptiveBloom: return "adaptive-bloom";
        case FilterKind::kLearnedBloom: return "learned-bloom";
        case FilterKind::kStandardBloom:
        default: return "standard-bloom";
        }
    }

} // namespace lsm
