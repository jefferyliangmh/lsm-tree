#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace lsm {

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

        // 块级读取与 Block Cache
        size_t      block_size = 4096;
        size_t      block_cache_capacity = 64ull * 1024 * 1024;
        int         block_cache_shard_bits = 4;

        // ★ Leveled Compaction 新增参数
        size_t      max_file_size = 2ull * 1024 * 1024;   // 单个 SST 目标大小
        size_t      max_bytes_for_level_base = 64ull * 1024 * 1024;  // L1 最大总大小
        double      max_bytes_for_level_multiplier = 10.0;             // 每层 ×10

        size_t      immutable_slowdown_threshold = 2;
        size_t      immutable_stop_threshold = 4;
        uint32_t    write_slowdown_delay_ms = 1;
        bool        collect_write_path_stats = false;
    };

} // namespace lsm