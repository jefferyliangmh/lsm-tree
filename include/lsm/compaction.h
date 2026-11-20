#pragma once
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

    private:
        Input BuildL0Compaction(const Version& v);
        Input BuildLevelCompaction(const Version& v, int level);
        void ExpandToCleanCut(const Version& v, int level,
            std::string* smallest, std::string* largest,
            std::vector<uint64_t>* files);
        static size_t TargetLevelSize(int level, const Options& opt);
        static size_t TotalSize(const std::vector<Version::FileMeta>& files);
    };

    // ★ 修改：Run 返回多个输出文件（Leveled 按 max_file_size 切分）
    class CompactionIO {
    public:
        virtual ~CompactionIO() = default;
        virtual Status Run(const CompactionPolicy::Input& input,
            const std::vector<std::shared_ptr<TableReader>>& readers,
            const std::string& output_dir,
            uint64_t first_output_number,
            const Options& opt,
            std::vector<VersionEdit::FileMeta>* outputs) = 0;
        virtual std::string Name() const = 0;
    };

    class SyncCompactionIO : public CompactionIO {
    public:
        Status Run(const CompactionPolicy::Input& input,
            const std::vector<std::shared_ptr<TableReader>>& readers,
            const std::string& output_dir,
            uint64_t first_output_number,
            const Options& opt,
            std::vector<VersionEdit::FileMeta>* outputs) override;
        std::string Name() const override { return "sync"; }
    };

} // namespace lsm