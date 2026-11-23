#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "lsm/options.h"
#include "lsm/sstable.h"
#include "lsm/version_set.h"

namespace lsm {

    class CompactionPolicy {
    public:
        virtual ~CompactionPolicy() = default;

        struct Input {
            int from_level = 0;
            int to_level = 1;
            std::vector<uint64_t> input_file_numbers;
            std::string smallest_ikey;
            std::string largest_ikey;
            bool is_bottommost = false;   // 是否是最底层（决定墓碑能否丢弃）
        };

        virtual std::vector<Input> PickCompaction(const Version& version,
            const Options& opt) = 0;
        virtual std::string Name() const = 0;
    };

    class LeveledPolicy : public CompactionPolicy {
    public:
        std::vector<Input> PickCompaction(const Version& version,
            const Options& opt) override;
        std::string Name() const override { return "leveled"; }

        friend class HybridPolicy;

    private:
        Input BuildL0Compaction(const Version& v);
        Input BuildLevelCompaction(const Version& v, int level);
        void ExpandToCleanCut(const Version& v, int level,
            std::string* smallest, std::string* largest,
            std::vector<uint64_t>* files);
        static size_t TargetLevelSize(int level, const Options& opt);
        static size_t TotalSize(const std::vector<Version::FileMeta>& files);
    };

    // 同层文件允许 key 重叠。达到 fanout 后把该层全部文件归并后晋升到下一层，
    // 不把下一层已有文件拉进输入（这是 size-tiered 降低写放大的原因）。
    class TieredPolicy : public CompactionPolicy {
    public:
        std::vector<Input> PickCompaction(const Version& version,
            const Options& opt) override;
        std::string Name() const override { return "size-tiered"; }
    };

    // 写密集：年轻层（< hybrid_leveled_from）走 tiered。
    // 读密集或深层：走 leveled，把重叠文件一并归并，恢复无重叠布局。
    class HybridPolicy : public CompactionPolicy {
    public:
        std::vector<Input> PickCompaction(const Version& version,
            const Options& opt) override;
        std::string Name() const override { return "hybrid"; }
    };

    std::unique_ptr<CompactionPolicy> MakeCompactionPolicy(CompactionStyle style);

    // Run 通过 allocate_file_number 现取文件号，避免在归并期间长时间占住
    // 文件号锁，从而让 flush 与 compaction 真正并行。
    class CompactionIO {
    public:
        virtual ~CompactionIO() = default;
        virtual Status Run(const CompactionPolicy::Input& input,
            const std::vector<std::shared_ptr<TableReader>>& readers,
            const std::string& output_dir,
            const std::function<Status(uint64_t*)>& allocate_file_number,
            const Options& opt,
            std::vector<VersionEdit::FileMeta>* outputs) = 0;
        virtual std::string Name() const = 0;
    };

    class SyncCompactionIO : public CompactionIO {
    public:
        Status Run(const CompactionPolicy::Input& input,
            const std::vector<std::shared_ptr<TableReader>>& readers,
            const std::string& output_dir,
            const std::function<Status(uint64_t*)>& allocate_file_number,
            const Options& opt,
            std::vector<VersionEdit::FileMeta>* outputs) override;
        std::string Name() const override { return "sync"; }
    };

    // 先用 AsyncIOBackend 把输入 SST 的数据块预取进 block cache，再执行归并。
    // 预取失败时降级为同步读，不改变归并结果。
    class PipelinedCompactionIO : public CompactionIO {
    public:
        Status Run(const CompactionPolicy::Input& input,
            const std::vector<std::shared_ptr<TableReader>>& readers,
            const std::string& output_dir,
            const std::function<Status(uint64_t*)>& allocate_file_number,
            const Options& opt,
            std::vector<VersionEdit::FileMeta>* outputs) override;
        std::string Name() const override { return "pipelined-async"; }

    private:
        SyncCompactionIO sync_;
    };

} // namespace lsm
