#include "lsm/compaction.h"
#include "lsm/async_io.h"
#include "lsm/internal_key.h"

#include <algorithm>
#include <cstdio>

namespace lsm {

    // ============================================================================
    // LeveledPolicy 辅助
    // ============================================================================

    size_t LeveledPolicy::TargetLevelSize(int level, const Options& opt) {
        double result = static_cast<double>(opt.max_bytes_for_level_base);
        for (int i = 1; i < level; ++i)
            result *= opt.max_bytes_for_level_multiplier;
        return static_cast<size_t>(result);
    }

    size_t LeveledPolicy::TotalSize(const std::vector<Version::FileMeta>& files) {
        size_t sum = 0;
        for (auto& f : files) sum += f.file_size;
        return sum;
    }

    void LeveledPolicy::ExpandToCleanCut(const Version& v, int level,
        std::string* smallest, std::string* largest,
        std::vector<uint64_t>* files) {
        if (level >= static_cast<int>(v.levels.size())) return;
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& f : v.levels[level]) {
                if (std::find(files->begin(), files->end(), f.number) != files->end())
                    continue;
                Slice file_smallest = UserKeyOf(Slice(f.smallest_ikey));
                Slice file_largest = UserKeyOf(Slice(f.largest_ikey));
                Slice range_smallest = UserKeyOf(Slice(*smallest));
                Slice range_largest = UserKeyOf(Slice(*largest));
                bool overlaps =
                    CompareUserKeys(file_smallest, range_largest) <= 0 &&
                    CompareUserKeys(file_largest, range_smallest) >= 0;
                if (!overlaps) continue;
                if (CompareInternal(Slice(f.smallest_ikey), Slice(*smallest)) < 0)
                    *smallest = f.smallest_ikey;
                if (CompareInternal(Slice(f.largest_ikey), Slice(*largest)) > 0)
                    *largest = f.largest_ikey;
                files->push_back(f.number);
                changed = true;
            }
        }
    }

    // ============================================================================
    // PickCompaction
    // ============================================================================

    static bool HasOverlappingFilesBelow(const Version& version, int level,
        const std::string& smallest, const std::string& largest) {
        for (size_t lower = static_cast<size_t>(level + 1);
            lower < version.levels.size(); ++lower) {
            for (const auto& file : version.levels[lower]) {
                Slice file_smallest = UserKeyOf(Slice(file.smallest_ikey));
                Slice file_largest = UserKeyOf(Slice(file.largest_ikey));
                if (CompareUserKeys(file_largest, UserKeyOf(Slice(smallest))) >= 0 &&
                    CompareUserKeys(file_smallest, UserKeyOf(Slice(largest))) <= 0)
                    return true;
            }
        }
        return false;
    }

    CompactionPolicy::Input BuildTiered(const Version& v, int level) {
        CompactionPolicy::Input in;
        in.from_level = level;
        const int last = static_cast<int>(v.levels.size()) - 1;
        if (last < 0 || level < 0 || level >= static_cast<int>(v.levels.size()) ||
            v.levels[level].empty())
            return in;
        in.to_level = (level < last) ? level + 1 : level;

        bool first = true;
        for (const auto& f : v.levels[level]) {
            in.input_file_numbers.push_back(f.number);
            if (first) {
                in.smallest_ikey = f.smallest_ikey;
                in.largest_ikey = f.largest_ikey;
                first = false;
                continue;
            }
            if (CompareInternal(Slice(f.smallest_ikey), Slice(in.smallest_ikey)) < 0)
                in.smallest_ikey = f.smallest_ikey;
            if (CompareInternal(Slice(f.largest_ikey), Slice(in.largest_ikey)) > 0)
                in.largest_ikey = f.largest_ikey;
        }

        bool foreign = false;
        if (in.to_level != in.from_level &&
            !v.Overlapping(in.to_level, in.smallest_ikey, in.largest_ikey).empty())
            foreign = true;
        if (HasOverlappingFilesBelow(v, in.to_level, in.smallest_ikey, in.largest_ikey))
            foreign = true;
        in.is_bottommost = !foreign;
        return in;
    }

    size_t TieredTrigger(int level, const Options& opt) {
        const int fanout = std::max(2, opt.tiered_fanout);
        if (level == 0)
            return static_cast<size_t>(std::max(2, std::min(fanout, std::max(1, opt.level0_trigger))));
        return static_cast<size_t>(fanout);
    }

    LeveledPolicy::Input LeveledPolicy::BuildL0Compaction(const Version& v) {
        Input in;
        in.from_level = 0;
        in.to_level = 1;

        auto& l0 = v.levels[0];
        in.smallest_ikey = l0[0].smallest_ikey;
        in.largest_ikey = l0[0].largest_ikey;
        for (auto& f : l0) {
            in.input_file_numbers.push_back(f.number);
            if (CompareInternal(Slice(f.smallest_ikey), Slice(in.smallest_ikey)) < 0)
                in.smallest_ikey = f.smallest_ikey;
            if (CompareInternal(Slice(f.largest_ikey), Slice(in.largest_ikey)) > 0)
                in.largest_ikey = f.largest_ikey;
        }

        auto l1_overlap = v.Overlapping(1, in.smallest_ikey, in.largest_ikey);
        for (auto& f : l1_overlap) in.input_file_numbers.push_back(f.number);
        if (!l1_overlap.empty())
            ExpandToCleanCut(v, 1, &in.smallest_ikey, &in.largest_ikey,
                &in.input_file_numbers);

        in.is_bottommost = !HasOverlappingFilesBelow(v, in.to_level,
            in.smallest_ikey, in.largest_ikey);
        return in;
    }

    LeveledPolicy::Input LeveledPolicy::BuildLevelCompaction(const Version& v,
        int level) {
        Input in;
        in.from_level = level;
        in.to_level = level + 1;

        auto& src = v.levels[level];
        const Version::FileMeta* seed = nullptr;
        for (auto& f : src) {
            if (!seed || f.number < seed->number) seed = &f;
        }
        if (!seed) return in;

        in.smallest_ikey = seed->smallest_ikey;
        in.largest_ikey = seed->largest_ikey;
        in.input_file_numbers.push_back(seed->number);
        ExpandToCleanCut(v, level, &in.smallest_ikey, &in.largest_ikey,
            &in.input_file_numbers);

        auto next_overlap = v.Overlapping(level + 1, in.smallest_ikey, in.largest_ikey);
        for (auto& f : next_overlap) in.input_file_numbers.push_back(f.number);
        if (!next_overlap.empty())
            ExpandToCleanCut(v, level + 1, &in.smallest_ikey, &in.largest_ikey,
                &in.input_file_numbers);

        in.is_bottommost = !HasOverlappingFilesBelow(v, in.to_level,
            in.smallest_ikey, in.largest_ikey);
        return in;
    }

    std::vector<CompactionPolicy::Input> LeveledPolicy::PickCompaction(
        const Version& v, const Options& opt) {
        std::vector<Input> result;
        if (v.levels.empty()) return result;

        if (v.levels[0].size() >= static_cast<size_t>(opt.level0_trigger)) {
            result.push_back(BuildL0Compaction(v));
            return result;
        }

        for (int level = 1; level + 1 < static_cast<int>(v.levels.size()); ++level) {
            size_t size = TotalSize(v.levels[level]);
            size_t target = TargetLevelSize(level, opt);
            if (size > target && !v.levels[level].empty()) {
                result.push_back(BuildLevelCompaction(v, level));
                return result;
            }
        }
        return result;
    }

    std::vector<CompactionPolicy::Input> TieredPolicy::PickCompaction(
        const Version& v, const Options& opt) {
        std::vector<Input> result;
        for (int level = 0; level < static_cast<int>(v.levels.size()); ++level) {
            if (v.levels[level].size() >= TieredTrigger(level, opt)) {
                result.push_back(BuildTiered(v, level));
                return result;
            }
        }
        return result;
    }

    std::vector<CompactionPolicy::Input> HybridPolicy::PickCompaction(
        const Version& v, const Options& opt) {
        std::vector<Input> result;
        if (v.levels.empty()) return result;
        const bool write_heavy =
            opt.observed_write_read_ratio >= opt.hybrid_write_heavy_ratio;
        auto use_tiered = [&](int level) {
            return write_heavy && level < opt.hybrid_leveled_from;
        };

        if (v.levels[0].size() >= static_cast<size_t>(std::max(1, opt.level0_trigger))) {
            if (use_tiered(0)) result.push_back(BuildTiered(v, 0));
            else {
                LeveledPolicy leveled;
                result.push_back(leveled.BuildL0Compaction(v));
            }
            return result;
        }

        for (int level = 1; level + 1 < static_cast<int>(v.levels.size()); ++level) {
            if (use_tiered(level) &&
                v.levels[level].size() >= TieredTrigger(level, opt)) {
                result.push_back(BuildTiered(v, level));
                return result;
            }
            const size_t size = LeveledPolicy::TotalSize(v.levels[level]);
            const size_t target = LeveledPolicy::TargetLevelSize(level, opt);
            if (size > target && !v.levels[level].empty()) {
                LeveledPolicy leveled;
                result.push_back(leveled.BuildLevelCompaction(v, level));
                return result;
            }
        }

        const int last = static_cast<int>(v.levels.size()) - 1;
        if (last > 0 && use_tiered(last) &&
            v.levels[last].size() >= TieredTrigger(last, opt)) {
            result.push_back(BuildTiered(v, last));
        }
        return result;
    }

    std::unique_ptr<CompactionPolicy> MakeCompactionPolicy(CompactionStyle style) {
        switch (style) {
        case CompactionStyle::kSizeTiered:
            return std::make_unique<TieredPolicy>();
        case CompactionStyle::kHybrid:
            return std::make_unique<HybridPolicy>();
        case CompactionStyle::kLeveled:
        default:
            return std::make_unique<LeveledPolicy>();
        }
    }

    // ============================================================================
    // SyncCompactionIO::Run：按 max_file_size 切分输出
    // ============================================================================

    Status SyncCompactionIO::Run(const CompactionPolicy::Input& input,
        const std::vector<std::shared_ptr<TableReader>>& readers,
        const std::string& output_dir,
        const std::function<Status(uint64_t*)>& allocate_file_number,
        const Options& opt,
        std::vector<VersionEdit::FileMeta>* outputs) {
        outputs->clear();
        if (readers.empty()) return Status::OK();
        if (!allocate_file_number)
            return Status::InvalidArgument("missing file number allocator");

        struct Cand { std::unique_ptr<TableReader::Iterator> it; };
        std::vector<Cand> cands;
        cands.reserve(readers.size());
        for (auto& r : readers) {
            if (!r) return Status::InvalidArgument("null compaction input reader");
            cands.push_back({ std::make_unique<TableReader::Iterator>(r.get()) });
            if (!cands.back().it->status().ok())
                return cands.back().it->status();
        }

        uint64_t out_number = 0;
        std::string out_path;
        Status alloc_st = allocate_file_number(&out_number);
        if (!alloc_st.ok()) return alloc_st;
        out_path = output_dir + "/" + std::to_string(out_number) + ".sst";
        auto builder = std::make_unique<TableBuilder>(out_path, opt);
        auto cleanup_outputs = [&] {
            builder.reset();
            (void)FileSystem::Default()->DeleteFile(out_path);
            for (const auto& output : *outputs) {
                (void)FileSystem::Default()->DeleteFile(output_dir + "/" +
                    std::to_string(output.file_number) + ".sst");
            }
            outputs->clear();
        };
        std::string out_smallest, out_largest;
        bool out_has_entry = false;

        std::string last_user_key;
        bool have_last = false;
        size_t total_written = 0;

        auto finalize_output = [&]() -> Status {
            if (!out_has_entry) {
                builder.reset();
                Status st = FileSystem::Default()->DeleteFile(out_path);
                return st.IsNotFound() ? Status::OK() : st;
            }
            Status st = builder->Finish();
            if (!st.ok()) return st;
            VersionEdit::FileMeta m;
            m.level = input.to_level;
            m.file_number = out_number;
            m.smallest_ikey = out_smallest;
            m.largest_ikey = out_largest;
            m.file_size = builder->ApproxSize();
            outputs->push_back(std::move(m));
            return Status::OK();
            };

        while (true) {
            int best = -1;
            for (size_t i = 0; i < cands.size(); ++i) {
                if (!cands[i].it->Valid()) continue;
                if (best < 0) { best = static_cast<int>(i); continue; }
                if (CompareInternal(cands[i].it->Key(),
                    cands[best].it->Key()) < 0)
                    best = static_cast<int>(i);
            }
            if (best < 0) break;

            auto& c = *cands[best].it;
            std::string uk = UserKeyOf(c.Key()).ToString();
            bool new_user_key = (!have_last || uk != last_user_key);

            if (new_user_key) {
                have_last = true;
                last_user_key = uk;

                if (builder->ApproxSize() >= opt.max_file_size) {
                    Status st = finalize_output();
                    if (!st.ok()) {
                        cleanup_outputs();
                        return st;
                    }

                    alloc_st = allocate_file_number(&out_number);
                    if (!alloc_st.ok()) {
                        cleanup_outputs();
                        return alloc_st;
                    }
                    out_path = output_dir + "/" + std::to_string(out_number) + ".sst";
                    builder = std::make_unique<TableBuilder>(out_path, opt);
                    out_has_entry = false;
                }
            }

            if (new_user_key) {
                if (c.Type() == ValueType::kValue) {
                    if (!out_has_entry) {
                        out_smallest.assign(c.Key().data(), c.Key().size());
                        out_has_entry = true;
                    }
                    out_largest.assign(c.Key().data(), c.Key().size());
                    builder->Add(c.Key(), c.Value());
                    ++total_written;
                }
                else if (!input.is_bottommost) {
                    if (!out_has_entry) {
                        out_smallest.assign(c.Key().data(), c.Key().size());
                        out_has_entry = true;
                    }
                    out_largest.assign(c.Key().data(), c.Key().size());
                    builder->Add(c.Key(), c.Value());
                    ++total_written;
                }
            }
            c.Next();
            if (!c.status().ok()) {
                Status st = c.status();
                cleanup_outputs();
                return st;
            }
        }

        Status st = finalize_output();
        if (!st.ok()) cleanup_outputs();
        return st;
    }

    Status PipelinedCompactionIO::Run(const CompactionPolicy::Input& input,
        const std::vector<std::shared_ptr<TableReader>>& readers,
        const std::string& output_dir,
        const std::function<Status(uint64_t*)>& allocate_file_number,
        const Options& opt,
        std::vector<VersionEdit::FileMeta>* outputs) {
        if (AsyncIOBackend* io = AsyncIOBackend::Default()) {
            for (const auto& reader : readers) {
                if (!reader) continue;
                Status st = reader->PrefetchDataBlocks(io);
                if (!st.ok()) {
                    std::fprintf(stderr,
                        "[pipelined] prefetch degraded to synchronous reads: %s\n",
                        st.ToString().c_str());
                }
            }
        }
        return sync_.Run(input, readers, output_dir, allocate_file_number, opt, outputs);
    }

} // namespace lsm