#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "lsm/crc32c.h"
#include "lsm/internal_key.h"
#include "lsm/status.h"
#include "lsm/env.h"

namespace lsm {

// WAL 记录：u32 len | u32 crc32c | payload
// payload：u8 type | u32 klen | u32 vlen | key | value
//
// GroupCommit：
//   - 多个写者把记录放入 pending_，取票号
//   - 无 Leader 时成为 Leader，取走整批一次 pwrite + 可选 fdatasync
//   - Leader 完成后设置 flushed_ticket_ = batch_max_ticket
//   - 等待者通过 cv_.wait 直到 flushed_ticket_ >= 自己的票号
class WAL {
public:
    struct Record {
        uint64_t    sequence = 0;
        ValueType   type;
        std::string key;
        std::string value;
    };

    struct WriteStats {
        uint64_t mutex_wait_ns = 0;
        uint64_t queue_wait_ns = 0;
        uint64_t append_io_ns = 0;
        uint64_t sync_ns = 0;
        uint64_t batches = 0;
        uint64_t records = 0;
    };

    WAL(std::string path, bool sync, uint64_t initial_sequence = 1);
    WAL(std::string path, bool sync, uint64_t initial_sequence,
        bool collect_write_path_stats);
    ~WAL();

    Status Append(const std::string& key, const std::string& value, ValueType type,
        uint64_t* sequence = nullptr);
    Status Recover(uint64_t last_sequence, std::vector<Record>* records);
    std::vector<Record> Recover();
    void SetNextSequence(uint64_t sequence);
    const Status& OpenStatus() const noexcept { return open_status_; }
    Status Reset();

    uint64_t BytesWritten() const noexcept {
        return bytes_.load(std::memory_order_relaxed);
    }
    WriteStats GetWriteStats() const noexcept;

private:
    struct PendingWrite {
        std::string record;
        uint64_t    ticket;
    };

    Status FlushBatch(const std::vector<PendingWrite>& batch);

    std::string path_;
    std::unique_ptr<WritableFile> file_;           // ★ 改为 Env 抽象
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool        sync_;
    bool        collect_write_path_stats_;
    std::atomic<uint64_t> bytes_{ 0 };
    std::atomic<uint64_t> mutex_wait_ns_{ 0 };
    std::atomic<uint64_t> queue_wait_ns_{ 0 };
    std::atomic<uint64_t> append_io_ns_{ 0 };
    std::atomic<uint64_t> sync_ns_{ 0 };
    std::atomic<uint64_t> batches_{ 0 };
    std::atomic<uint64_t> records_{ 0 };

    std::vector<PendingWrite> pending_;
    bool        leader_active_ = false;
    uint64_t    next_ticket_ = 0;
    uint64_t    flushed_ticket_ = 0;
    uint64_t    failed_ticket_ = 0;
    Status      last_error_;
    Status      open_status_;
    uint64_t    next_sequence_ = 1;
};

} // namespace lsm
