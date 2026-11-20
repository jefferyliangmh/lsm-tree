#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "lsm/slice.h"

namespace lsm {

// ============================================================================
// BlockHandle：指向 SSTable 中一个块的 (offset, size)
// 对标 LevelDB 的 BlockHandle / RocksDB 的 BlockHandle。
// ============================================================================
struct BlockHandle {
    uint64_t offset = 0;
    uint64_t size   = 0;
};

// ============================================================================
// BlockBuilder：构建一个数据块
//
// 块格式（LevelDB 兼容）：
//   [entry 0][entry 1]...[entry N-1][restart_0]...[restart_R-1][num_restarts u32]
//   每条 entry：
//     varint32 shared_len | varint32 non_shared_len | varint32 value_len
//     | non_shared_key | value
//   每 block_restart_interval 条记录建立一个 restart point（shared_len=0）
//
// restart point 让块内可以二分查找，而不是线性扫描。
// ============================================================================
class BlockBuilder {
public:
    explicit BlockBuilder(size_t restart_interval = 16)
        : restart_interval_(restart_interval) {
        restarts_.push_back(0);   // 第一个 restart 在 offset 0
    }

    void Add(const Slice& key, const Slice& value);
    Slice Finish();
    size_t CurrentSizeEstimate() const;
    void Reset();
    bool Empty() const { return buffer_.empty(); }

private:
    void AddRestartPoint();

    std::string buffer_;
    std::vector<uint32_t> restarts_;
    size_t restart_interval_;
    size_t counter_ = 0;        // 自上次 restart 以来的 entry 数
    std::string last_key_;
    bool finished_ = false;
};

// ============================================================================
// Block：解析一个已完成的数据块，提供块内二分查找
//
// 生命周期由 BlockCache 的引用计数管理；调用者拿到 Block* 后
// 必须保证在使用期间 BlockCache 不会驱逐它（通过 Ref/Unref）。
// ============================================================================
class Block {
public:
    // data 必须是完整的块内容（不含 5 字节 trailer）
    explicit Block(std::string data);

    // 在块内查找第一个 user_key >= target 的 entry
    // key 由调用者提供稳定存储：内部重建的完整内部键写入 *key，
    // value 指向块内数据（data_ 存活期间有效）
    bool Seek(const Slice& target, std::string* key, Slice* value) const;

    // 块内条目数（粗略估计，用于诊断）
    size_t NumRestarts() const { return num_restarts_; }
    size_t size() const { return data_.size(); }

private:
    // 一次解析返回所有需要的信息
    // 返回 false 时输出参数不写；调用者必须在 false 时提前返回
    bool DecodeEntry(uint32_t offset, uint32_t* shared,
        uint32_t* non_shared, uint32_t* value_len,
        uint32_t* key_offset, uint32_t* next_offset) const;

    std::string data_;
    uint32_t restarts_offset_ = 0;
    uint32_t num_restarts_ = 0;
};

// ============================================================================
// 变长整数编解码（LevelDB varint32）
// ============================================================================
void PutVarint32(std::string* dst, uint32_t value);
bool GetVarint32(std::string_view* input, uint32_t* value);

} // namespace lsm