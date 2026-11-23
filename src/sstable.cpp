#include "lsm/sstable.h"
#include "lsm/async_io.h"
#include "lsm/crc32c.h"
#include "lsm/env.h"
#include "lsm/metrics.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace lsm {

    // 防溢出的范围检查
    static inline bool InRange(uint64_t off, uint64_t size, uint64_t limit) {
        return off <= limit && size <= limit - off;
    }

    // 从文件名解析 file_number：".../123.sst" → 123
    static uint64_t ParseFileNumberFromPath(const std::string& path) {
        size_t slash = path.find_last_of('/');
        size_t dot = path.find_last_of('.');
        if (slash == std::string::npos || dot == std::string::npos || dot <= slash)
            return 0;
        try {
            return std::stoull(path.substr(slash + 1, dot - slash - 1));
        }
        catch (...) {
            return 0;
        }
    }

    // ============================================================================
    // TableBuilder
    // ============================================================================

    TableBuilder::TableBuilder(const std::string& path, const Options& opt)
        : path_(path), opt_(opt),
        data_block_(opt.block_restart_interval),
        bloom_(opt.bloom_bits_per_key) {
        Status st = FileSystem::Default()->NewWritableFile(path, &file_);
        if (!st.ok()) error_ = st;
    }

    TableBuilder::~TableBuilder() = default;

    void TableBuilder::Add(const Slice& ikey, const Slice& value) {
        if (!file_ || !error_.ok()) return;

        if (ikey.size() < 8) {
            error_ = Status::InvalidArgument("internal key too short");
            return;
        }

        if (data_block_.Empty()) {
            pending_first_key_.assign(ikey.data(), ikey.size());
        }
        pending_last_key_.assign(ikey.data(), ikey.size());
        data_block_.Add(ikey, value);

        if (opt_.use_bloom) {
            Slice uk = UserKeyOf(ikey);
            bloom_key_offsets_.push_back({ bloom_key_buffer_.size(), uk.size() });
            bloom_key_buffer_.append(uk.data(), uk.size());
        }

        ++entries_;

        if (data_block_.CurrentSizeEstimate() >= opt_.block_size) {
            Status st = FlushDataBlock();
            if (!st.ok()) error_ = st;
        }
    }

    Status TableBuilder::FlushDataBlock() {
        if (data_block_.Empty()) return Status::OK();
        if (!file_) return Status::IOError("table file not open");

        Slice raw = data_block_.Finish();
        BlockHandle h;
        h.offset = offset_;
        h.size = raw.size();

        Status st = file_->AppendAt(raw, offset_);
        if (!st.ok()) return st;
        offset_ += raw.size();

        char trailer[kBlockTrailerSize];
        trailer[0] = 0;
        uint32_t crc = CRC32C::Compute(raw.data(), raw.size());
        std::memcpy(trailer + 1, &crc, 4);
        st = file_->AppendAt(Slice(trailer, kBlockTrailerSize), offset_);
        if (!st.ok()) return st;
        offset_ += kBlockTrailerSize;

        index_.push_back({ pending_first_key_, h });
        data_blocks_.push_back(h);
        data_block_.Reset();
        return Status::OK();
    }

    Status TableBuilder::Finish() {
        if (finished_) return error_;
        finished_ = true;
        if (!file_) return Status::IOError("table file not open");
        if (!error_.ok()) return error_;

        Status st = FlushDataBlock();
        if (!st.ok()) return st;

        // Bloom
        if (opt_.use_bloom && !bloom_key_offsets_.empty()) {
            size_t n = bloom_key_offsets_.size();
            std::vector<const char*> ptrs(n);
            std::vector<size_t> lens(n);
            for (size_t i = 0; i < n; ++i) {
                ptrs[i] = bloom_key_buffer_.data() + bloom_key_offsets_[i].first;
                lens[i] = bloom_key_offsets_[i].second;
            }
            std::vector<uint64_t> h64s(n);
            BloomHash::HashBatch(ptrs.data(), lens.data(), n, h64s.data());
            for (uint64_t h : h64s) bloom_.AddRaw(h);
        }

        std::string bloom_data;
        if (opt_.use_bloom) {
            bloom_data = bloom_.Finish();
            if (opt_.filter_kind == FilterKind::kLearnedBloom) {
                bloom_data = WrapLearnedBloom(bloom_data, bloom_key_buffer_,
                    bloom_key_offsets_);
            }
        }
        uint64_t bloom_off = offset_;
        uint64_t bloom_size = bloom_data.size();
        if (!bloom_data.empty()) {
            st = file_->AppendAt(Slice(bloom_data), offset_);
            if (!st.ok()) return st;
            offset_ += bloom_data.size();
        }

        // Index
        uint64_t index_off = offset_;
        for (const auto& e : index_) {
            uint32_t klen = static_cast<uint32_t>(e.ikey.size());
            std::string buf;
            buf.reserve(4 + 8 + 8 + klen);
            buf.append(reinterpret_cast<const char*>(&klen), 4);
            buf.append(reinterpret_cast<const char*>(&e.handle.offset), 8);
            buf.append(reinterpret_cast<const char*>(&e.handle.size), 8);
            buf.append(e.ikey);
            st = file_->AppendAt(Slice(buf), offset_);
            if (!st.ok()) return st;
            offset_ += buf.size();
        }
        uint64_t index_size = offset_ - index_off;

        // Footer
        uint32_t magic = kSSTMagic;
        char footer[44];
        std::memcpy(footer, &index_off, 8);
        std::memcpy(footer + 8, &index_size, 8);
        std::memcpy(footer + 16, &bloom_off, 8);
        std::memcpy(footer + 24, &bloom_size, 8);
        std::memcpy(footer + 32, &magic, 4);
        st = file_->AppendAt(Slice(footer, 44), offset_);
        if (!st.ok()) return st;
        offset_ += 44;

        st = file_->Sync();
        if (!st.ok()) return st;

        file_.reset();

        bloom_key_buffer_.clear();
        bloom_key_buffer_.shrink_to_fit();
        bloom_key_offsets_.clear();
        bloom_key_offsets_.shrink_to_fit();
        return Status::OK();
    }

    // ============================================================================
    // TableReader
    // ============================================================================

    TableReader::~TableReader() = default;

    std::shared_ptr<TableReader> TableReader::Open(
        const std::string& path, std::shared_ptr<ShardedLRUCache> cache) {
        uint64_t file_number = ParseFileNumberFromPath(path);

        auto r = std::shared_ptr<TableReader>(
            new TableReader(path, file_number, std::move(cache)));

        Status st = FileSystem::Default()->NewRandomAccessFile(path, &r->file_);
        if (!st.ok()) return nullptr;

        r->file_size_ = r->file_->Size();

        if (!r->ParseMeta()) return nullptr;
        return r;
    }

    bool TableReader::ParseMeta() {
        if (file_size_ < 44) return false;
        if (!file_) return false;

        char footer[44];
        Slice result;
        Status st = file_->Read(file_size_ - 44, 44, &result, footer);
        if (!st.ok() || result.size() != 44) return false;

        uint64_t index_off, index_size, bloom_off, bloom_size;
        uint32_t magic;
        std::memcpy(&index_off, footer, 8);
        std::memcpy(&index_size, footer + 8, 8);
        std::memcpy(&bloom_off, footer + 16, 8);
        std::memcpy(&bloom_size, footer + 24, 8);
        std::memcpy(&magic, footer + 32, 4);
        if (magic != kSSTMagic) return false;

        if (!InRange(index_off, index_size, file_size_)) return false;
        if (!InRange(bloom_off, bloom_size, file_size_)) return false;

        bloom_off_ = bloom_off;
        bloom_size_ = bloom_size;
        data_end_ = bloom_off;
        if (data_end_ > file_size_) data_end_ = file_size_;

        if (bloom_size > 0) {
            std::string bloom_data(bloom_size, '\0');
            st = file_->Read(bloom_off, bloom_size, &result, bloom_data.data());
            if (!st.ok() || result.size() != bloom_size) return false;
            bloom_ = BloomFilter(std::move(bloom_data));
        }

        if (index_size > 0) {
            std::string index_data(index_size, '\0');
            st = file_->Read(index_off, index_size, &result, index_data.data());
            if (!st.ok() || result.size() != index_size) return false;

            const char* p = index_data.data();
            const char* lim = p + index_size;
            while (p + 20 <= lim) {
                uint32_t klen;
                BlockHandle h;
                std::memcpy(&klen, p, 4);
                std::memcpy(&h.offset, p + 4, 8);
                std::memcpy(&h.size, p + 12, 8);
                p += 20;

                if (static_cast<uint64_t>(klen) > static_cast<uint64_t>(lim - p))
                    break;
                if (h.size > (std::numeric_limits<uint64_t>::max)() - kBlockTrailerSize ||
                    !InRange(h.offset, h.size + kBlockTrailerSize, data_end_))
                    return false;
                index_.push_back({ std::string(p, klen), h });
                p += klen;
            }
        }

        num_entries_ = index_.size();
        return true;
    }

    Status TableReader::ReadBlock(const BlockHandle& h,
        std::shared_ptr<Block>* block) const {
        block->reset();
        if (!file_) return Status::IOError("SST file is not open: " + path_);
        if (h.size > SIZE_MAX - kBlockTrailerSize ||
            !InRange(h.offset, h.size + kBlockTrailerSize, file_size_))
            return Status::Corruption("data block is out of range: " + path_);

        ShardedLRUCache::CacheKey ck{ file_number_, h.offset };
        if (cache_) {
            *block = cache_->Lookup(ck);
            if (*block) {
                EngineCounters::block_cache_hits.fetch_add(1, std::memory_order_relaxed);
                return Status::OK();
            }
        }

        std::string data(h.size + kBlockTrailerSize, '\0');
        Slice result;
        Status st = file_->Read(h.offset, data.size(), &result, data.data());
        if (!st.ok()) return st;
        if (result.size() != data.size())
            return Status::Corruption("truncated data block: " + path_);
        if (data[h.size] != 0)
            return Status::Corruption("unsupported data block compression: " + path_);

        uint32_t stored_crc;
        std::memcpy(&stored_crc, data.data() + h.size + 1, 4);
        uint32_t actual_crc = CRC32C::Compute(data.data(), h.size);
        if (stored_crc != actual_crc)
            return Status::Corruption("data block checksum mismatch: " + path_);

        EngineCounters::block_reads.fetch_add(1, std::memory_order_relaxed);
        EngineCounters::block_read_bytes.fetch_add(data.size(), std::memory_order_relaxed);

        data.resize(h.size);
        if (cache_) {
            cache_->Insert(ck, data);
            *block = cache_->Lookup(ck);
        }
        if (!*block) *block = std::make_shared<Block>(std::move(data));
        return Status::OK();
    }

    size_t TableReader::FindIndexPos(const Slice& user_key) const {
        if (index_.empty()) return 0;

        size_t lo = 0, hi = index_.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            Slice mid_user = UserKeyOf(Slice(index_[mid].ikey));
            if (mid_user.compare(user_key) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo == 0 ? 0 : lo - 1;
    }

    TableReader::Result TableReader::Get(const Slice& user_key) const {
        Result res;
        if (telemetry_) telemetry_->lookups.fetch_add(1, std::memory_order_relaxed);
        if (!bloom_.Empty() && !bloom_.MightContain(user_key)) {
            if (telemetry_) telemetry_->bloom_rejects.fetch_add(1, std::memory_order_relaxed);
            return res;
        }
        const bool bloom_said_yes = !bloom_.Empty();
        if (bloom_said_yes && telemetry_)
            telemetry_->bloom_hits.fetch_add(1, std::memory_order_relaxed);

        size_t idx = FindIndexPos(user_key);
        if (idx >= index_.size()) {
            if (bloom_said_yes && telemetry_)
                telemetry_->false_positives.fetch_add(1, std::memory_order_relaxed);
            return res;
        }

        std::shared_ptr<Block> block;
        res.status = ReadBlock(index_[idx].handle, &block);
        if (!res.status.ok()) return res;

        std::string probe;
        probe.reserve(user_key.size() + 8);
        probe.append(user_key.data(), user_key.size());
        char seq_buf[8];
        EncodeFixed64BE(seq_buf,
            ((std::numeric_limits<uint64_t>::max)() >> 8) << 8);
        probe.append(seq_buf, 8);

        std::string found_key;
        Slice found_value;
        bool seek_ok = block->Seek(Slice(probe), &found_key, &found_value);

        if (!seek_ok && idx + 1 < index_.size()) {
            std::shared_ptr<Block> next;
            res.status = ReadBlock(index_[idx + 1].handle, &next);
            if (!res.status.ok()) return res;
            if (next->Seek(Slice(probe), &found_key, &found_value)) {
                block = std::move(next);
                seek_ok = true;
            }
        }
        if (!seek_ok) {
            if (bloom_said_yes && telemetry_)
                telemetry_->false_positives.fetch_add(1, std::memory_order_relaxed);
            return res;
        }

        if (UserKeyOf(Slice(found_key)).compare(user_key) != 0) {
            if (bloom_said_yes && telemetry_)
                telemetry_->false_positives.fetch_add(1, std::memory_order_relaxed);
            return res;
        }

        res.found = true;
        res.value.assign(found_value.data(), found_value.size());
        uint64_t tag = DecodeFixed64BE(found_key.data() + found_key.size() - 8);
        res.type = static_cast<ValueType>(tag & 0xFF);
        if (telemetry_) telemetry_->found.fetch_add(1, std::memory_order_relaxed);
        return res;
    }

    Status TableReader::PrefetchDataBlocks(AsyncIOBackend* backend) const {
        if (!backend || !file_ || index_.empty()) return Status::OK();

        struct Pending {
            BlockHandle handle;
            std::string buf;
        };
        std::vector<Pending> pending;
        pending.reserve(index_.size());
        for (const auto& entry : index_) {
            if (cache_) {
                ShardedLRUCache::CacheKey ck{ file_number_, entry.handle.offset };
                if (cache_->Lookup(ck)) continue;
            }
            if (entry.handle.size > SIZE_MAX - kBlockTrailerSize ||
                !InRange(entry.handle.offset,
                    entry.handle.size + kBlockTrailerSize, file_size_))
                continue;
            Pending item;
            item.handle = entry.handle;
            item.buf.resize(entry.handle.size + kBlockTrailerSize);
            pending.push_back(std::move(item));
        }

        constexpr size_t kWindow = 32;
        for (size_t start = 0; start < pending.size(); start += kWindow) {
            const size_t n = std::min(kWindow, pending.size() - start);
            std::vector<AsyncIOBackend::Request> reqs;
            reqs.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                auto& item = pending[start + i];
                AsyncIOBackend::Request req;
                req.file = file_.get();
                req.offset = item.handle.offset;
                req.buf = item.buf.data();
                req.len = item.buf.size();
                req.user_data = i;
                reqs.push_back(req);
            }
            Status st = backend->Submit(reqs);
            if (!st.ok()) return st;
            std::vector<uint64_t> done;
            std::vector<Status> statuses;
            st = backend->Wait(static_cast<unsigned>(n), &done, &statuses);
            if (!st.ok()) return st;
            if (done.size() != n || statuses.size() != n)
                return Status::IOError("prefetch completion count mismatch");

            std::vector<int> order(n, -1);
            for (size_t k = 0; k < n; ++k) {
                if (done[k] >= n)
                    return Status::IOError("prefetch tag out of range");
                order[done[k]] = static_cast<int>(k);
            }
            for (size_t i = 0; i < n; ++i) {
                if (order[i] < 0 || !statuses[static_cast<size_t>(order[i])].ok())
                    continue;
                auto& item = pending[start + i];
                if (item.buf.size() != item.handle.size + kBlockTrailerSize) continue;
                if (item.buf[item.handle.size] != 0) continue;
                uint32_t stored_crc = 0;
                std::memcpy(&stored_crc, item.buf.data() + item.handle.size + 1, 4);
                const uint32_t actual = CRC32C::Compute(item.buf.data(), item.handle.size);
                if (stored_crc != actual) continue;
                item.buf.resize(item.handle.size);
                EngineCounters::prefetch_blocks.fetch_add(1, std::memory_order_relaxed);
                EngineCounters::prefetch_bytes.fetch_add(item.buf.size(),
                    std::memory_order_relaxed);
                if (cache_) {
                    ShardedLRUCache::CacheKey ck{ file_number_, item.handle.offset };
                    cache_->Insert(ck, std::move(item.buf));
                }
            }
        }
        return Status::OK();
    }

    // ============================================================================
    // TableReader::Iterator
    // ============================================================================

    TableReader::Iterator::Iterator(const TableReader* r) : r_(r) {
        if (!r_->file_) {
            status_ = Status::IOError("SST file is not open: " + r_->path_);
            return;
        }
        valid_ = AdvanceEntry();
    }

    TableReader::Iterator::~Iterator() = default;

    bool TableReader::Iterator::LoadBlock() {
        if (block_idx_ >= r_->index_.size()) return false;

        const auto& h = r_->index_[block_idx_].handle;
        if (h.size > SIZE_MAX - kBlockTrailerSize ||
            !InRange(h.offset, h.size + kBlockTrailerSize, r_->file_size_)) {
            status_ = Status::Corruption("data block is out of range: " + r_->path_);
            return false;
        }
        if (h.size < 4) {
            status_ = Status::Corruption("data block is too small: " + r_->path_);
            return false;
        }

        buf_.resize(h.size + kBlockTrailerSize);
        Slice result;
        Status st = r_->file_->Read(h.offset, buf_.size(), &result, buf_.data());
        if (!st.ok()) {
            status_ = st;
            return false;
        }
        if (result.size() != buf_.size()) {
            status_ = Status::Corruption("truncated data block: " + r_->path_);
            return false;
        }
        if (buf_[h.size] != 0) {
            status_ = Status::Corruption("unsupported data block compression: " + r_->path_);
            return false;
        }
        uint32_t stored_crc;
        std::memcpy(&stored_crc, buf_.data() + h.size + 1, 4);
        if (stored_crc != CRC32C::Compute(buf_.data(), h.size)) {
            status_ = Status::Corruption("data block checksum mismatch: " + r_->path_);
            return false;
        }
        buf_.resize(h.size);

        uint32_t num_restarts = 0;
        std::memcpy(&num_restarts, buf_.data() + buf_.size() - 4, 4);
        if (num_restarts == 0 || num_restarts > (buf_.size() - 4) / 4) {
            status_ = Status::Corruption("invalid restart array: " + r_->path_);
            return false;
        }
        restarts_offset_ = static_cast<uint32_t>(
            buf_.size() - 4 - num_restarts * 4);

        offset_ = 0;
        prev_key_.clear();
        ++block_idx_;
        return true;
    }

    bool TableReader::Iterator::AdvanceEntry() {
        while (true) {
            if (offset_ >= restarts_offset_) {
                if (!LoadBlock()) return false;
            }

            std::string_view input(buf_.data() + offset_,
                restarts_offset_ - offset_);
            uint32_t shared = 0, non_shared = 0, value_len = 0;
            if (!GetVarint32(&input, &shared) ||
                !GetVarint32(&input, &non_shared) ||
                !GetVarint32(&input, &value_len)) {
                status_ = Status::Corruption("invalid data block entry header: " + r_->path_);
                return false;
            }

            size_t header_len = static_cast<size_t>(
                input.data() - (buf_.data() + offset_));
            if (offset_ + header_len + non_shared + value_len > restarts_offset_) {
                status_ = Status::Corruption("data block entry exceeds block: " + r_->path_);
                return false;
            }

            std::string new_key;
            if (shared > 0) {
                if (shared > prev_key_.size()) {
                    status_ = Status::Corruption("invalid shared key prefix: " + r_->path_);
                    return false;
                }
                new_key.assign(prev_key_.data(), shared);
            }
            new_key.append(buf_.data() + offset_ + header_len, non_shared);

            if (new_key.size() < 8) {
                status_ = Status::Corruption("invalid internal key in data block: " + r_->path_);
                return false;
            }

            val_.assign(buf_.data() + offset_ + header_len + non_shared, value_len);

            uint64_t tag = DecodeFixed64BE(new_key.data() + new_key.size() - 8);
            type_ = static_cast<ValueType>(tag & 0xFF);

            prev_key_ = new_key;
            key_ = std::move(new_key);

            offset_ += static_cast<uint32_t>(header_len + non_shared + value_len);
            return true;
        }
    }

    void TableReader::Iterator::Next() {
        if (!valid_) return;
        valid_ = AdvanceEntry();
    }

    // ============================================================================
    // TableReader::OpenBatch
    //
    // Missing SST references produce null result entries for recovery repair.
    // I/O failures and corrupt SSTs return a non-OK status to preserve MANIFEST.
    //
    // 4 阶段并行：
    //   1. 打开所有 RandomAccessFile（缺失的标记 skipped）
    //   2. 并行读所有 Footer
    //   3. 并行读所有 Bloom+Index
    //   4. 逐个构造 TableReader（skipped 的跳过）
    //
    // 如果 valid_count == 0，直接返回 OK，results 全 nullptr。
    // ============================================================================

    // ============================================================================
// TableReader::OpenBatch
//
// Missing SST references are skipped; I/O failures and corrupt files are errors.
// Footer and index/bloom data are read in separate overlapped batches.
// ============================================================================
    Status TableReader::OpenBatch(
        const std::vector<std::string>& paths,
        std::shared_ptr<ShardedLRUCache> cache,
        std::vector<std::shared_ptr<TableReader>>* results) {

        results->assign(paths.size(), nullptr);
        if (paths.empty()) return Status::OK();

        struct Ctx {
            std::unique_ptr<RandomAccessFile> file;
            uint64_t file_number = 0;
            uint64_t file_size = 0;
            uint64_t index_off = 0, index_size = 0;
            uint64_t bloom_off = 0, bloom_size = 0;
            std::string payload_buf;
        };
        std::vector<Ctx> ctxs(paths.size());
        std::vector<bool> skipped(paths.size(), false);
        std::vector<uint64_t> missing_numbers;
        auto* fs = FileSystem::Default();

        // -------------------------------------------------------------------
        // 阶段 1：FileExists 预检 + 打开文件
        //
        // 缺失的文件只消耗一次 stat 系统调用（~10渭s），
        // 而不是一次 open 系统调用（~1ms）。
        // -------------------------------------------------------------------
        size_t valid_count = 0;
        for (size_t i = 0; i < paths.size(); ++i) {
            ctxs[i].file_number = ParseFileNumberFromPath(paths[i]);

            Status exists = fs->FileExists(paths[i]);
            if (exists.IsNotFound()) {
                skipped[i] = true;
                missing_numbers.push_back(ctxs[i].file_number);
                continue;
            }
            if (!exists.ok())
                return Status::IOError("check SST existence " + paths[i] + ": " +
                    exists.ToString());

            Status st = fs->NewRandomAccessFile(paths[i], &ctxs[i].file);
            if (st.IsNotFound()) {
                skipped[i] = true;
                missing_numbers.push_back(ctxs[i].file_number);
                continue;
            }
            if (!st.ok())
                return Status::IOError("open SST " + paths[i] + ": " + st.ToString());

            ctxs[i].file_size = ctxs[i].file->Size();
            if (ctxs[i].file_size < 44)
                return Status::Corruption("SST is too small: " + paths[i]);
            ++valid_count;
        }

        // 汇总日志：一行打印所有缺失，而不是逐行
        if (!missing_numbers.empty()) {
            std::fprintf(stderr,
                "[OpenBatch] %zu missing: [", missing_numbers.size());
            for (size_t i = 0; i < missing_numbers.size(); ++i) {
                std::fprintf(stderr, "%s%llu",
                    i == 0 ? "" : ",",
                    (unsigned long long)missing_numbers[i]);
            }
            std::fprintf(stderr, "]\n");
        }

        if (valid_count == 0) {
            std::fprintf(stderr, "[OpenBatch] no valid files (all %zu missing)\n",
                paths.size());
            return Status::OK();
        }

        auto* backend = AsyncIOBackend::Default();

        auto fallback = [&]() -> Status {
            size_t loaded = 0;
            for (size_t i = 0; i < paths.size(); ++i) {
                if (skipped[i]) continue;
                (*results)[i] = Open(paths[i], cache);
                if (!(*results)[i])
                    return Status::Corruption("cannot parse SST: " + paths[i]);
                ++loaded;
            }
            std::fprintf(stderr, "[OpenBatch] fallback loaded %zu/%zu\n",
                loaded, paths.size());
            return Status::OK();
        };

        auto run_batch = [&](const std::vector<AsyncIOBackend::Request>& reqs,
            const char* stage) -> Status {
            if (reqs.empty()) return Status::OK();
            Status st = backend->Submit(reqs);
            if (!st.ok()) return Status::IOError(std::string(stage) + " submit: " +
                st.ToString());

            std::vector<uint64_t> completed;
            std::vector<Status> statuses;
            st = backend->Wait(static_cast<unsigned>(reqs.size()), &completed, &statuses);
            if (!st.ok()) return Status::IOError(std::string(stage) + " wait: " +
                st.ToString());
            if (completed.size() != reqs.size() || statuses.size() != reqs.size())
                return Status::IOError(std::string(stage) + " returned incomplete results");

            for (size_t i = 0; i < statuses.size(); ++i) {
                if (completed[i] >= paths.size())
                    return Status::Corruption(std::string(stage) +
                        " returned an invalid file index");
                if (!statuses[i].ok())
                    return Status::IOError(std::string(stage) + " read " +
                        paths[static_cast<size_t>(completed[i])] + ": " +
                        statuses[i].ToString());
            }
            return Status::OK();
        };

        if (!backend) return fallback();

        // -------------------------------------------------------------------
        // 阶段 2：并行读 Footer
        // -------------------------------------------------------------------
        {
            std::vector<AsyncIOBackend::Request> reqs;
            std::vector<std::string> footers(paths.size());
            reqs.reserve(valid_count);
            for (size_t i = 0; i < paths.size(); ++i) {
                if (skipped[i]) continue;
                footers[i].resize(44);
                AsyncIOBackend::Request req;
                req.file = ctxs[i].file.get();
                req.offset = ctxs[i].file_size - 44;
                req.buf = footers[i].data();
                req.len = 44;
                req.user_data = static_cast<uint64_t>(i);
                reqs.push_back(std::move(req));
            }

            Status read_st = run_batch(reqs, "footer");
            if (!read_st.ok()) return read_st;

            for (size_t i = 0; i < paths.size(); ++i) {
                if (skipped[i]) continue;
                const char* f = footers[i].data();
                uint32_t magic;
                std::memcpy(&ctxs[i].index_off, f, 8);
                std::memcpy(&ctxs[i].index_size, f + 8, 8);
                std::memcpy(&ctxs[i].bloom_off, f + 16, 8);
                std::memcpy(&ctxs[i].bloom_size, f + 24, 8);
                std::memcpy(&magic, f + 32, 4);
                if (magic != kSSTMagic ||
                    !InRange(ctxs[i].index_off, ctxs[i].index_size, ctxs[i].file_size) ||
                    !InRange(ctxs[i].bloom_off, ctxs[i].bloom_size, ctxs[i].file_size))
                    return Status::Corruption("invalid SST footer: " + paths[i]);
            }
        }

        // -------------------------------------------------------------------
        // 阶段 3：并行读 Bloom + Index
        // -------------------------------------------------------------------
        {
            std::vector<AsyncIOBackend::Request> reqs;
            reqs.reserve(valid_count);
            std::vector<uint64_t> payload_starts(paths.size(), 0);

            for (size_t i = 0; i < paths.size(); ++i) {
                if (skipped[i]) continue;
                uint64_t start = std::min(ctxs[i].bloom_off, ctxs[i].index_off);
                uint64_t end = std::max(ctxs[i].bloom_off + ctxs[i].bloom_size,
                    ctxs[i].index_off + ctxs[i].index_size);
                if (end < start || end - start > (std::numeric_limits<size_t>::max)())
                    return Status::Corruption("invalid SST payload range: " + paths[i]);
                size_t len = static_cast<size_t>(end - start);
                payload_starts[i] = start;
                ctxs[i].payload_buf.resize(len);
                if (len == 0) continue;

                AsyncIOBackend::Request req;
                req.file = ctxs[i].file.get();
                req.offset = start;
                req.buf = ctxs[i].payload_buf.data();
                req.len = len;
                req.user_data = static_cast<uint64_t>(i);
                reqs.push_back(std::move(req));
            }

            Status read_st = run_batch(reqs, "index/bloom");
            if (!read_st.ok()) return read_st;

            size_t loaded = 0;
            for (size_t i = 0; i < paths.size(); ++i) {
                if (skipped[i]) continue;

                auto tr = std::shared_ptr<TableReader>(
                    new TableReader(paths[i], ctxs[i].file_number, cache,
                        std::move(ctxs[i].file)));
                tr->file_size_ = ctxs[i].file_size;
                tr->bloom_off_ = ctxs[i].bloom_off;
                tr->bloom_size_ = ctxs[i].bloom_size;
                tr->data_end_ = std::min<uint64_t>(ctxs[i].bloom_off,
                    ctxs[i].file_size);

                if (!tr->InitFromPayload(ctxs[i].payload_buf, payload_starts[i],
                    ctxs[i].bloom_off, ctxs[i].bloom_size,
                    ctxs[i].index_off, ctxs[i].index_size))
                    return Status::Corruption("invalid SST index or bloom data: " + paths[i]);
                (*results)[i] = std::move(tr);
                ++loaded;
            }

            std::fprintf(stderr, "[OpenBatch] loaded %zu/%zu\n",
                loaded, paths.size());
        }

        return Status::OK();
    }

    // ============================================================================
    // TableReader::InitFromPayload
    //
    // payload_buf 是 [payload_start, payload_start + payload_buf.size())
    // 的内容；bloom 和 index 是其中的子区间，按绝对偏移 - payload_start 定位。
    // ============================================================================
    bool TableReader::InitFromPayload(const std::string& payload_buf,
        uint64_t payload_start,
        uint64_t bloom_off, uint64_t bloom_size,
        uint64_t index_off, uint64_t index_size) {
        if (bloom_size > 0) {
            if (bloom_off < payload_start) return false;
            uint64_t rel = bloom_off - payload_start;
            if (rel > payload_buf.size() || bloom_size > payload_buf.size() - rel)
                return false;
            bloom_ = BloomFilter(payload_buf.substr(rel, bloom_size));
        }

        if (index_size > 0) {
            if (index_off < payload_start) return false;
            uint64_t rel = index_off - payload_start;
            if (rel > payload_buf.size() || index_size > payload_buf.size() - rel)
                return false;

            const char* p = payload_buf.data() + rel;
            const char* lim = p + index_size;
            while (p + 20 <= lim) {
                uint32_t klen;
                BlockHandle h;
                std::memcpy(&klen, p, 4);
                std::memcpy(&h.offset, p + 4, 8);
                std::memcpy(&h.size, p + 12, 8);
                p += 20;

                if (static_cast<uint64_t>(klen) >
                    static_cast<uint64_t>(lim - p))
                    break;
                if (h.size > (std::numeric_limits<uint64_t>::max)() - kBlockTrailerSize ||
                    !InRange(h.offset, h.size + kBlockTrailerSize, data_end_))
                    return false;
                index_.push_back({ std::string(p, klen), h });
                p += klen;
            }
        }

        num_entries_ = index_.size();
        return true;
    }

} // namespace lsm