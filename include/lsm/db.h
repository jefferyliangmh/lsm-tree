#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>
#include "lsm/block_cache.h"
#include "lsm/compaction.h"
#include "lsm/memtable.h"
#include "lsm/options.h"
#include "lsm/sstable.h"
#include "lsm/status.h"
#include "lsm/version.h"
#include "lsm/version_set.h"
#include "lsm/wal.h"

namespace lsm {

    struct WritePathStats {
        uint64_t capacity_wait_ns = 0;
        uint64_t write_lock_wait_ns = 0;
        uint64_t wal_append_ns = 0;
        uint64_t memtable_lock_wait_ns = 0;
        uint64_t memtable_add_ns = 0;
        uint64_t rotation_ns = 0;
        uint64_t wal_mutex_wait_ns = 0;
        uint64_t wal_queue_wait_ns = 0;
        uint64_t wal_append_io_ns = 0;
        uint64_t wal_sync_ns = 0;
        uint64_t wal_batches = 0;
        uint64_t wal_records = 0;
        uint64_t background_flush_ns = 0;
        uint64_t background_flushes = 0;
    };

    struct Stats {
        uint64_t puts = 0, gets = 0, flushes = 0, compactions = 0;
        WritePathStats write_path;
        size_t memtable_entries = 0;
        size_t memtable_arena_bytes = 0;
        size_t immutable_memtables = 0;
        size_t immutable_memtables_peak = 0;
        uint64_t write_slowdowns = 0;
        uint64_t write_stops = 0;
        std::vector<size_t> level_files;
    };

    class DB {
    public:
        explicit DB(Options opt);
        ~DB();

        Status Put(const std::string& key, const std::string& value);
        Status Delete(const std::string& key);
        Status Get(const std::string& key, std::string* value);
        Status Flush();
        Status Compact();

        Stats GetStats() const;
        std::string CompactionPolicyName() const { return policy_->Name(); }

    private:
        Status WaitForWriteCapacity();
        Status RotateMemtable();
        Status FlushOldestImmutable();
        Status CleanupObsoleteWals(uint64_t minimum_log_number);
        void   ScheduleFlush();
        Status CompactLocked();
        Status DoCompaction(const CompactionPolicy::Input& input);
        void   BackgroundLoop();
        void   CompactionLoop();
        void   ScheduleCompaction();
        void   PublishSuperVersionLocked();
        void   AdvanceFailedSequence(uint64_t sequence);

        // ★ 成员声明顺序即构造顺序：block_cache_ 必须在 versions_ 之前
        Options opt_;
        std::atomic<uint64_t> seq_{ 0 };

        mutable std::mutex memtable_mu_;          // ★ mutable：GetStats() const 需要
        struct ImmutableMemTable {
            std::shared_ptr<MemTable> memtable;
            uint64_t minimum_log_number = 0;
            uint64_t last_sequence = 0;
        };
        std::shared_ptr<MemTable> mutable_mem_;
        std::vector<ImmutableMemTable> immutable_mems_;
        uint64_t mutable_log_number_ = 0;
        uint64_t current_wal_number_ = 0;

        mutable std::shared_mutex write_mu_;
        std::mutex flush_mu_;
        std::mutex file_number_mu_;
        std::condition_variable memtable_cv_;

        std::unique_ptr<WAL> wal_;

        std::shared_ptr<ShardedLRUCache> block_cache_;   // ★ 在 versions_ 之前
        VersionSet versions_;                             // ★ 用初始化列表构造
        VersionManager version_mgr_;

        std::unique_ptr<CompactionPolicy> policy_;
        std::unique_ptr<CompactionIO> compaction_io_;

        std::atomic<bool> shutdown_{ false };
        std::thread bg_;
        std::condition_variable compaction_cv_;
        std::atomic<bool> compaction_pending_{ false };
        std::thread compaction_thread_;
        std::mutex background_mu_;
        std::condition_variable background_cv_;
        std::atomic<bool> flush_pending_{ false };

        struct AtomicStats {
            std::atomic<uint64_t> puts{ 0 };
            std::atomic<uint64_t> gets{ 0 };
            std::atomic<uint64_t> flushes{ 0 };
            std::atomic<uint64_t> compactions{ 0 };
            std::atomic<uint64_t> write_slowdowns{ 0 };
            std::atomic<uint64_t> write_stops{ 0 };
            std::atomic<uint64_t> capacity_wait_ns{ 0 };
            std::atomic<uint64_t> write_lock_wait_ns{ 0 };
            std::atomic<uint64_t> wal_append_ns{ 0 };
            std::atomic<uint64_t> memtable_lock_wait_ns{ 0 };
            std::atomic<uint64_t> memtable_add_ns{ 0 };
            std::atomic<uint64_t> rotation_ns{ 0 };
            std::atomic<uint64_t> wal_mutex_wait_ns{ 0 };
            std::atomic<uint64_t> wal_queue_wait_ns{ 0 };
            std::atomic<uint64_t> wal_append_io_ns{ 0 };
            std::atomic<uint64_t> wal_sync_ns{ 0 };
            std::atomic<uint64_t> wal_batches{ 0 };
            std::atomic<uint64_t> wal_records{ 0 };
            std::atomic<uint64_t> background_flush_ns{ 0 };
            std::atomic<uint64_t> background_flushes{ 0 };
            std::atomic<size_t> immutable_memtables_peak{ 0 };
        };
        mutable AtomicStats stats_;
    };

} // namespace lsm