#include "lsm/version_set.h"
#include "lsm/env.h"
#include "lsm/crc32c.h"
#include "lsm/metrics.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <map>

namespace lsm {

    namespace {
        constexpr uint32_t kManifestV2Flag = 0x80000000u;
        constexpr uint8_t kManifestRecordVersion = 2;

        Status EncodeManifestRecord(const VersionEdit& edit, std::string* record) {
            std::string payload(1, static_cast<char>(kManifestRecordVersion));
            payload.append(edit.Encode());
            if (payload.size() >= kManifestV2Flag)
                return Status::InvalidArgument("MANIFEST record is too large");

            uint32_t length = static_cast<uint32_t>(payload.size()) | kManifestV2Flag;
            uint32_t crc = CRC32C::Compute(payload.data(), payload.size());
            record->assign(reinterpret_cast<const char*>(&length), sizeof(length));
            record->append(reinterpret_cast<const char*>(&crc), sizeof(crc));
            record->append(payload);
            return Status::OK();
        }

        bool DecodeManifestRecord(const char* data, size_t size,
            bool versioned, VersionEdit* edit) {
            if (size == 0) return false;
            if (versioned) {
                if (static_cast<uint8_t>(data[0]) != kManifestRecordVersion)
                    return false;
                return edit->Decode(data + 1, size - 1);
            }
            return edit->Decode(data, size);
        }
    }

    static bool ParseNumberedFile(const std::string& name,
        const std::string& prefix, const std::string& suffix, uint64_t* number) {
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            return false;

        const char* begin = name.data() + prefix.size();
        const char* end = name.data() + name.size() - suffix.size();
        auto parsed = std::from_chars(begin, end, *number);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    static Status UpdateNextFileNumber(const std::vector<std::string>& children,
        uint64_t* next_file_number) {
        uint64_t highest = 0;
        for (const auto& name : children) {
            uint64_t number = 0;
            if (ParseNumberedFile(name, "", ".sst", &number) ||
                ParseNumberedFile(name, "MANIFEST-", "", &number))
                highest = std::max(highest, number);
        }
        if (highest == (std::numeric_limits<uint64_t>::max)())
            return Status::Corruption("file number space exhausted");
        *next_file_number = std::max(*next_file_number, highest + 1);
        return Status::OK();
    }

    // ============================================================================
    // Version
    // ============================================================================

    void Version::AddFile(int level, const VersionEdit::FileMeta& f,
        std::shared_ptr<TableReader> reader) {
        if (level >= static_cast<int>(levels.size())) levels.resize(level + 1);

        FileMeta m{ f.file_number, f.smallest_ikey, f.largest_ikey, f.file_size,
                   std::move(reader) };
        levels[level].push_back(std::move(m));

        if (level == 0) {
            std::sort(levels[level].begin(), levels[level].end(),
                [](const FileMeta& a, const FileMeta& b) {
                    return a.number > b.number;
                });
        }
        else {
            std::sort(levels[level].begin(), levels[level].end(),
                [](const FileMeta& a, const FileMeta& b) {
                    return CompareInternal(Slice(a.smallest_ikey),
                        Slice(b.smallest_ikey)) < 0;
                });
        }
    }

    void Version::RemoveFile(int level, uint64_t number) {
        if (level >= static_cast<int>(levels.size())) return;
        auto& v = levels[level];
        v.erase(std::remove_if(v.begin(), v.end(),
            [number](const FileMeta& f) {
                return f.number == number;
            }),
            v.end());
    }

    std::vector<Version::FileMeta> Version::Overlapping(
        int level, const std::string& smallest, const std::string& largest) const {
        std::vector<FileMeta> out;
        if (level >= static_cast<int>(levels.size())) return out;
        Slice smallest_user = UserKeyOf(Slice(smallest));
        Slice largest_user = UserKeyOf(Slice(largest));
        for (auto& f : levels[level]) {
            Slice file_smallest_user = UserKeyOf(Slice(f.smallest_ikey));
            Slice file_largest_user = UserKeyOf(Slice(f.largest_ikey));
            if (CompareUserKeys(file_largest_user, smallest_user) >= 0 &&
                CompareUserKeys(file_smallest_user, largest_user) <= 0) {
                out.push_back(f);
            }
        }
        return out;
    }

    // ============================================================================
    // VersionSet
    // ============================================================================

    VersionSet::VersionSet(std::string db_path, const Options& opt,
        std::shared_ptr<ShardedLRUCache> cache)
        : db_path_(std::move(db_path)), opt_(opt), cache_(std::move(cache)) {
        current_ = std::make_shared<Version>();
        current_->levels.resize(opt_.max_levels);
    }

    VersionSet::~VersionSet() = default;

    Status VersionSet::Recover() {
        std::lock_guard lk(mu_);
        auto fs = FileSystem::Default();

        std::string current_path = db_path_ + "/CURRENT";

        Status current_exists = fs->FileExists(current_path);
        if (current_exists.IsNotFound()) {
            std::vector<std::string> children;
            Status children_st = fs->GetChildren(db_path_, &children);
            if (!children_st.ok()) return children_st;
            for (const auto& name : children) {
                uint64_t number = 0;
                if (ParseNumberedFile(name, "", ".sst", &number) ||
                    ParseNumberedFile(name, "MANIFEST-", "", &number))
                    return Status::Corruption("database metadata exists without CURRENT");
            }
            return WriteSnapshotLocked();
        }
        if (!current_exists.ok()) return current_exists;

        // 读 CURRENT
        std::string manifest_name;
        {
            std::unique_ptr<RandomAccessFile> rf;
            Status st = fs->NewRandomAccessFile(current_path, &rf);
            if (!st.ok()) return st;
            uint64_t size = rf->Size();
            if (size == 0 || size > 256)
                return Status::Corruption("bad CURRENT size");
            std::string buf(size, '\0');
            Slice result;
            st = rf->Read(0, size, &result, buf.data());
            if (!st.ok()) return st;
            if (result.size() != size)
                return Status::IOError("short read from CURRENT");
            manifest_name = std::move(buf);
            while (!manifest_name.empty() &&
                (manifest_name.back() == '\n' || manifest_name.back() == '\r'))
                manifest_name.pop_back();
            if (!ParseNumberedFile(manifest_name, "MANIFEST-", "", &manifest_number_))
                return Status::Corruption("invalid MANIFEST name in CURRENT");
        }

        // 读 MANIFEST
        std::string manifest_path = db_path_ + "/" + manifest_name;
        std::unique_ptr<RandomAccessFile> mrf;
        Status st = fs->NewRandomAccessFile(manifest_path, &mrf);
        if (!st.ok()) return Status::IOError("cannot open MANIFEST: " + manifest_path);

        uint64_t manifest_size = mrf->Size();
        if (manifest_size == 0 || manifest_size > (std::numeric_limits<size_t>::max)())
            return Status::Corruption("invalid MANIFEST size");
        std::string data(static_cast<size_t>(manifest_size), '\0');
        Slice result;
        st = mrf->Read(0, manifest_size, &result, data.data());
        if (!st.ok()) return st;
        if (result.size() != manifest_size)
            return Status::IOError("short read from MANIFEST");

        // ========================================================================
        // 解码 VersionEdit，用 map 维护最终状态
        //
        // 关键修复：
        //   AddFile → final_files[num] = meta
        //   DeleteFile → final_files.erase(num)
        //
        // 解码完成后，final_files 只包含当前有效的 SST（不含被 Compaction 删除的）。
        // 上一版用 vector 累积所有历史 AddFile，没有剔除 DeleteFile，导致
        // OpenBatch 去打开已被删除的文件。
        // ========================================================================
        std::map<uint64_t, VersionEdit::FileMeta> final_files;

        size_t p = 0;
        size_t decoded = 0;
        while (p < data.size()) {
            if (data.size() - p < sizeof(uint32_t))
                return Status::Corruption("truncated MANIFEST record header");
            uint32_t header;
            std::memcpy(&header, data.data() + p, sizeof(header));
            p += sizeof(header);
            bool versioned = (header & kManifestV2Flag) != 0;
            uint32_t len = versioned ? (header & ~kManifestV2Flag) : header;
            uint32_t expected_crc = 0;
            if (versioned) {
                if (data.size() - p < sizeof(expected_crc))
                    return Status::Corruption("truncated MANIFEST checksum");
                std::memcpy(&expected_crc, data.data() + p, sizeof(expected_crc));
                p += sizeof(expected_crc);
            }
            if (len > data.size() - p)
                return Status::Corruption("truncated MANIFEST record");
            if (versioned && CRC32C::Compute(data.data() + p, len) != expected_crc)
                return Status::Corruption("MANIFEST checksum mismatch");

            VersionEdit edit;
            if (!DecodeManifestRecord(data.data() + p, len, versioned, &edit))
                return Status::Corruption("invalid MANIFEST record");
            p += len;
            ++decoded;

            if (edit.HasLastSequence()) last_sequence_ = edit.LastSequence();
            if (edit.HasLogNumber())    log_number_ = edit.LogNumber();
            if (edit.HasNextFileNumber())
                next_file_number_ = std::max(next_file_number_, edit.NextFileNumber());

            for (auto& df : edit.DeletedFiles())
                final_files.erase(df.second);
            for (auto& af : edit.AddedFiles())
                final_files[af.file_number] = af;
        }
        if (decoded == 0)
            return Status::Corruption("MANIFEST contains no valid records");

        std::vector<std::string> children;
        st = fs->GetChildren(db_path_, &children);
        if (!st.ok()) return st;
        st = UpdateNextFileNumber(children, &next_file_number_);
        if (!st.ok()) return st;
        reserved_file_number_end_ = next_file_number_;

        // ========================================================================
        // 从 final_files 构建最终待打开列表
        // ========================================================================
        std::vector<VersionEdit::FileMeta> pending_metas;
        std::vector<std::string> paths;
        pending_metas.reserve(final_files.size());
        paths.reserve(final_files.size());
        for (auto& [num, meta] : final_files) {
            pending_metas.push_back(meta);
            paths.push_back(db_path_ + "/" + std::to_string(num) + ".sst");
        }

        // ========================================================================
        // OpenBatch 并行打开最终 SST（只有 6 个，不再有 28 个 SKIP）
        // ========================================================================
        std::vector<VersionEdit::FileMeta> missing_metas;

        if (!pending_metas.empty()) {
            std::vector<std::shared_ptr<TableReader>> readers;
            Status open_st = TableReader::OpenBatch(paths, cache_, &readers);
            if (!open_st.ok()) {
                std::fprintf(stderr, "[Recover] OpenBatch hard failure: %s\n",
                    open_st.ToString().c_str());
                return open_st;
            }

            for (size_t i = 0; i < pending_metas.size(); ++i) {
                if (!readers[i]) {
                    missing_metas.push_back(pending_metas[i]);
                    continue;
                }
                readers[i]->SetFilterTelemetry(filter_telemetry_);
                current_->AddFile(pending_metas[i].level, pending_metas[i],
                    std::move(readers[i]));
            }
        }

        if (!missing_metas.empty())
            return Status::Corruption("MANIFEST references missing SST files");

        // ========================================================================
        // 诊断日志
        // ========================================================================
        std::fprintf(stderr,
            "[Recover] decoded %zu records, seq=%llu, loaded=%zu, missing=%zu\n",
            decoded, (unsigned long long)last_sequence_,
            pending_metas.size() - missing_metas.size(), missing_metas.size());
        for (size_t i = 0; i < current_->levels.size(); ++i) {
            if (!current_->levels[i].empty())
                std::fprintf(stderr, "[Recover]   L%zu: %zu files\n",
                    i, current_->levels[i].size());
        }

        // ========================================================================
        // 打开 MANIFEST 用于后续追加
        // ========================================================================
        st = fs->NewAppendableFile(manifest_path, &manifest_file_);
        if (!st.ok()) return Status::IOError("cannot reopen MANIFEST for append");

        // ========================================================================
        // 孤儿清理：仅删除当前 Version 未引用的严格编号 SST。
        // ========================================================================
        std::set<uint64_t> live;
        for (const auto& level : current_->levels)
            for (const auto& file : level) live.insert(file.number);

        std::vector<std::string> cleanup_children;
        st = fs->GetChildren(db_path_, &cleanup_children);
        if (!st.ok()) return st;
        for (const auto& name : cleanup_children) {
            uint64_t number = 0;
            if (!ParseNumberedFile(name, "", ".sst", &number) ||
                live.count(number))
                continue;
            st = fs->DeleteFile(db_path_ + "/" + name);
            if (!st.ok() && !st.IsNotFound()) return st;
        }

        return Status::OK();
    }

    // ============================================================================
    // LogAndApply：加锁包装
    // ============================================================================
    Status VersionSet::LogAndApply(
        VersionEdit* edit,
        const std::vector<std::shared_ptr<TableReader>>& readers) {
        std::lock_guard lk(mu_);
        return LogAndApplyLocked(edit, readers);
    }

    // ============================================================================
    // LogAndApplyLocked：无锁实现，调用者已持有 mu_
    // ============================================================================
    Status VersionSet::LogAndApplyLocked(
        VersionEdit* edit,
        const std::vector<std::shared_ptr<TableReader>>& readers) {
        if (readers.size() != edit->AddedFiles().size())
            return Status::InvalidArgument("reader count mismatch");

        if (commit_state_uncertain_)
            return Status::IOError("MANIFEST commit state is uncertain; reopen database");
        Status st = AppendEditLocked(*edit);
        if (!st.ok()) return st;

        auto new_version = std::make_shared<Version>(*current_);

        for (auto& df : edit->DeletedFiles())
            new_version->RemoveFile(df.first, df.second);
        for (size_t i = 0; i < edit->AddedFiles().size(); ++i) {
            auto& af = edit->AddedFiles()[i];
            if (readers[i]) readers[i]->SetFilterTelemetry(filter_telemetry_);
            new_version->AddFile(af.level, af, readers[i]);
        }

        if (edit->HasLastSequence()) last_sequence_ = edit->LastSequence();
        if (edit->HasLogNumber())    log_number_ = edit->LogNumber();
        if (edit->HasNextFileNumber())
            reserved_file_number_end_ = std::max(reserved_file_number_end_,
                edit->NextFileNumber());
        current_ = std::move(new_version);
        return Status::OK();

    }

    // ============================================================================
    // WriteSnapshotLocked：无锁实现，调用者已持有 mu_
    // ============================================================================
    Status VersionSet::WriteSnapshotLocked() {
        auto fs = FileSystem::Default();

        manifest_number_ = next_file_number_++;
        std::string manifest_path = db_path_ + "/MANIFEST-" +
            std::to_string(manifest_number_);
        std::unique_ptr<WritableFile> mf;
        Status st = fs->NewWritableFile(manifest_path, &mf);
        if (!st.ok()) return st;

        VersionEdit edit;
        edit.SetLastSequence(0);
        edit.SetLogNumber(0);
        edit.SetNextFileNumber(next_file_number_);
        std::string record;
        st = EncodeManifestRecord(edit, &record);
        if (!st.ok()) return st;

        st = mf->Append(Slice(record));
        if (!st.ok()) return st;
        st = mf->Sync();
        if (!st.ok()) return st;
        st = fs->SyncDir(db_path_);
        if (!st.ok()) return st;

        manifest_file_ = std::move(mf);
        reserved_file_number_end_ = next_file_number_;

        return SetCurrentFileLocked(manifest_number_);
    }

    // ============================================================================
    // SetCurrentFileLocked：无锁实现，调用者已持有 mu_
    // ============================================================================
    Status VersionSet::SetCurrentFileLocked(uint64_t manifest_number) {
        auto fs = FileSystem::Default();

        std::string tmp = db_path_ + "/CURRENT.tmp";
        std::string dst = db_path_ + "/CURRENT";
        std::string name = "MANIFEST-" + std::to_string(manifest_number);

        std::unique_ptr<WritableFile> f;
        Status st = fs->NewWritableFile(tmp, &f);
        if (!st.ok()) return st;
        st = f->Append(Slice(name));
        if (!st.ok()) return st;
        st = f->Sync();
        if (!st.ok()) return st;
        f.reset();

        st = fs->RenameFile(tmp, dst);
        if (!st.ok()) return st;

        return fs->SyncDir(db_path_);
    }

    Status VersionSet::AppendEditLocked(const VersionEdit& edit) {
        if (!manifest_file_) return Status::IOError("MANIFEST not open");
        std::string record;
        Status st = EncodeManifestRecord(edit, &record);
        if (!st.ok()) return st;
        st = manifest_file_->Append(Slice(record));
        if (!st.ok()) {
            commit_state_uncertain_ = true;
            return st;
        }
        st = manifest_file_->Sync();
        if (!st.ok()) {
            commit_state_uncertain_ = true;
            return st;
        }
        return Status::OK();
    }

    Status VersionSet::NewFileNumber(uint64_t* number) {
        if (!number) return Status::InvalidArgument("null file number output");
        std::lock_guard lk(mu_);
        if (commit_state_uncertain_)
            return Status::IOError("MANIFEST commit state is uncertain; reopen database");
        if (next_file_number_ == (std::numeric_limits<uint64_t>::max)())
            return Status::Corruption("file number space exhausted");
        if (next_file_number_ >= reserved_file_number_end_) {
            constexpr uint64_t kReservationSize = 64;
            if (next_file_number_ > (std::numeric_limits<uint64_t>::max)() -
                kReservationSize)
                return Status::Corruption("file number reservation overflow");
            VersionEdit reservation;
            reservation.SetNextFileNumber(next_file_number_ + kReservationSize);
            Status st = LogAndApplyLocked(&reservation, {});
            if (!st.ok()) return st;
        }
        *number = next_file_number_++;
        return Status::OK();
    }

    bool VersionSet::CommitStateUncertain() const {
        std::lock_guard lk(mu_);
        return commit_state_uncertain_;
    }

    std::shared_ptr<const Version> VersionSet::Current() const {
        std::lock_guard lk(mu_);
        return current_;
    }

    uint64_t VersionSet::LastSequence() const { std::lock_guard lk(mu_); return last_sequence_; }
    uint64_t VersionSet::LogNumber() const { std::lock_guard lk(mu_); return log_number_; }

} // namespace lsm