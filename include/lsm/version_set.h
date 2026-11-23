#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "lsm/block_cache.h"
#include "lsm/env.h"
#include "lsm/options.h"
#include "lsm/sstable.h"
#include "lsm/status.h"
#include "lsm/version_edit.h"

namespace lsm {

    struct FilterTelemetry;

    class Version {
    public:
        struct FileMeta {
            uint64_t number;
            std::string smallest_ikey;
            std::string largest_ikey;
            uint64_t file_size;
            std::shared_ptr<TableReader> reader;
        };
        std::vector<std::vector<FileMeta>> levels;

        void AddFile(int level, const VersionEdit::FileMeta& f,
            std::shared_ptr<TableReader> reader);
        void RemoveFile(int level, uint64_t number);
        std::vector<FileMeta> Overlapping(int level,
            const std::string& smallest,
            const std::string& largest) const;
    };

    class VersionSet {
    public:
        VersionSet(std::string db_path, const Options& opt,
            std::shared_ptr<ShardedLRUCache> cache);
        ~VersionSet();

        VersionSet(const VersionSet&) = delete;
        VersionSet& operator=(const VersionSet&) = delete;

        Status Recover();

        // 加锁包装：外部调用者使用
        Status LogAndApply(VersionEdit* edit,
            const std::vector<std::shared_ptr<TableReader>>& readers);

        std::shared_ptr<const Version> Current() const;

        uint64_t LastSequence() const;
        uint64_t LogNumber() const;

        Status NewFileNumber(uint64_t* number);
        bool CommitStateUncertain() const;
        void SetFilterTelemetry(FilterTelemetry* telemetry) noexcept {
            filter_telemetry_ = telemetry;
        }

    private:
        // ★ 无锁实现：调用者必须已持有 mu_
        Status LogAndApplyLocked(
            VersionEdit* edit,
            const std::vector<std::shared_ptr<TableReader>>& readers);

        Status WriteSnapshotLocked();
        Status SetCurrentFileLocked(uint64_t manifest_number);
        Status AppendEditLocked(const VersionEdit& edit);

        std::string db_path_;
        Options opt_;
        std::shared_ptr<ShardedLRUCache> cache_;

        mutable std::mutex mu_;
        std::shared_ptr<Version> current_;
        uint64_t last_sequence_ = 0;
        uint64_t log_number_ = 0;
        uint64_t next_file_number_ = 1;
        uint64_t reserved_file_number_end_ = 1;
        uint64_t manifest_number_ = 1;
        bool commit_state_uncertain_ = false;
        std::unique_ptr<WritableFile> manifest_file_;
        FilterTelemetry* filter_telemetry_ = nullptr;
    };

} // namespace lsm