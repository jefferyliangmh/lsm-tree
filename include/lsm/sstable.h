#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "lsm/block.h"
#include "lsm/block_cache.h"
#include "lsm/bloom.h"
#include "lsm/env.h"
#include "lsm/internal_key.h"
#include "lsm/options.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

    class AsyncIOBackend;
    struct FilterTelemetry;

    // ============================================================================
    // SSTable 布局：
    //   [DataBlock 0][DataBlock 1]...[DataBlock N-1]
    //   [Bloom Block]
    //   [Index Block]      : 每项 u32 klen | u64 block_offset | u64 block_size | ikey
    //   [Footer 44B]       : u64 index_off | u64 index_size
    //                        u64 bloom_off | u64 bloom_size | u32 magic
    //
    // 每个 DataBlock 尾部有 5 字节 trailer：
    //   u8 compression_type(恒 0) | u32 crc32c
    // ============================================================================

    constexpr uint32_t kSSTMagic = 0x4c534d53u;
    constexpr size_t   kBlockTrailerSize = 5;

    // ============================================================================
    // TableBuilder：构建 SSTable
    // ============================================================================
    class TableBuilder {
    public:
        TableBuilder(const std::string& path, const Options& opt);
        ~TableBuilder();

        void Add(const Slice& ikey, const Slice& value);
        Status Finish();

        size_t   NumEntries() const noexcept { return entries_; }
        uint64_t ApproxSize() const noexcept { return offset_; }

    private:
        struct IndexEntry {
            std::string ikey;
            BlockHandle handle;
        };

        Status FlushDataBlock();

        std::string                   path_;
        Options                       opt_;
        std::unique_ptr<WritableFile> file_;

        BlockBuilder                  data_block_;
        std::string                   pending_first_key_;
        std::string                   pending_last_key_;
        std::vector<IndexEntry>       index_;
        std::vector<BlockHandle>      data_blocks_;

        BloomBuilder                  bloom_;
        std::string                   bloom_key_buffer_;
        std::vector<std::pair<size_t, size_t>> bloom_key_offsets_;

        uint64_t                      offset_ = 0;
        size_t                        entries_ = 0;
        bool                          finished_ = false;
        Status                        error_;
    };

    // ============================================================================
    // TableReader：读取 SSTable
    // ============================================================================
    class TableReader {
    public:
        static std::shared_ptr<TableReader> Open(
            const std::string& path,
            std::shared_ptr<ShardedLRUCache> cache);

        // ========================================================================
        // OpenBatch：批量并行打开，用于 VersionSet::Recover
        //
        // 内部用 AsyncIOBackend 一次提交所有 Footer 读取，一次提交所有
        // Bloom+Index 读取。15 个 SST 从 45 次同步 pread 降到 2 次批量异步 I/O。
        //
        // results 与 paths 一一对应；任何失败导致整个批次失败（不返回部分结果）。
        // AsyncIOBackend 不可用或失败时，回退到逐个 Open。
        // ========================================================================
        static Status OpenBatch(
            const std::vector<std::string>& paths,
            std::shared_ptr<ShardedLRUCache> cache,
            std::vector<std::shared_ptr<TableReader>>* results);

        ~TableReader();

        struct Result {
            Status      status;
            bool        found = false;
            std::string value;
            ValueType   type = ValueType::kValue;
        };

        Result Get(const Slice& user_key) const;

        // 把尚未缓存的数据块通过 AsyncIOBackend 读入 block cache。
        // 单个块失败不会使整个预取失败；Submit/Wait 失败则返回错误。
        Status PrefetchDataBlocks(AsyncIOBackend* backend) const;

        void SetFilterTelemetry(FilterTelemetry* telemetry) noexcept {
            telemetry_ = telemetry;
        }

        class Iterator {
        public:
            explicit Iterator(const TableReader* r);
            ~Iterator();
            Iterator(const Iterator&) = delete;
            Iterator& operator=(const Iterator&) = delete;

            bool Valid() const noexcept { return valid_; }
            Slice Key()      const noexcept { return { key_.data(), key_.size() }; }
            Slice Value()    const noexcept { return { val_.data(), val_.size() }; }
            ValueType Type() const noexcept { return type_; }
            const Status& status() const noexcept { return status_; }
            void Next();

        private:
            bool AdvanceEntry();
            bool LoadBlock();

            const TableReader* r_;
            std::string buf_;
            uint32_t    restarts_offset_ = 0;
            size_t      block_idx_ = 0;
            uint32_t    offset_ = 0;
            std::string prev_key_;
            std::string key_, val_;
            ValueType   type_ = ValueType::kValue;
            Status      status_;
            bool        valid_ = false;
        };

        Iterator NewIterator() const { return Iterator(this); }

        size_t NumEntries() const noexcept { return num_entries_; }
        uint64_t FileSize() const noexcept { return file_size_; }
        uint64_t FileNumber() const noexcept { return file_number_; }
        const std::string& Path() const noexcept { return path_; }

    private:
        struct IndexEntry {
            std::string ikey;
            BlockHandle handle;
        };

        // 普通构造：不打开文件（由 Open 设置 file_）
        TableReader(std::string path, uint64_t file_number,
            std::shared_ptr<ShardedLRUCache> cache)
            : path_(std::move(path)), file_number_(file_number),
            cache_(std::move(cache)) {
        }

        // 批量构造：直接持有已打开的 file_（OpenBatch 用）
        TableReader(std::string path, uint64_t file_number,
            std::shared_ptr<ShardedLRUCache> cache,
            std::unique_ptr<RandomAccessFile> file)
            : path_(std::move(path)), file_(std::move(file)),
            file_number_(file_number), cache_(std::move(cache)) {
        }

        bool ParseMeta();

        // ★ 从已读好的 buffer 初始化 bloom_ 和 index_，不进行 I/O
        //   payload_buf 是 [payload_start, payload_start + payload_len) 的字节，
        //   内部按 bloom_off / index_off 相对于 payload_start 的偏移拆分
        bool InitFromPayload(const std::string& payload_buf,
            uint64_t payload_start,
            uint64_t bloom_off, uint64_t bloom_size,
            uint64_t index_off, uint64_t index_size);

        size_t FindIndexPos(const Slice& user_key) const;
        Status ReadBlock(const BlockHandle& handle,
            std::shared_ptr<Block>* block) const;

        std::string                       path_;
        std::unique_ptr<RandomAccessFile> file_;
        uint64_t                          file_number_ = 0;
        uint64_t                          file_size_ = 0;
        std::shared_ptr<ShardedLRUCache>  cache_;

        BloomFilter             bloom_;
        std::vector<IndexEntry> index_;
        uint64_t                bloom_off_ = 0;
        uint64_t                bloom_size_ = 0;
        uint64_t                data_end_ = 0;
        uint64_t                num_entries_ = 0;
        FilterTelemetry*        telemetry_ = nullptr;
    };

} // namespace lsm