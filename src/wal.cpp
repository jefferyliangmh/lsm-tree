#include "lsm/wal.h"
#include "lsm/crc32c.h"
#include "lsm/env.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>

namespace lsm {

    namespace {
        constexpr uint32_t kWalV2Flag = 0x80000000u;
        constexpr uint8_t kWalV2Marker = 0xFF;
        constexpr uint8_t kWalV2Version = 2;

        void PutU32LE(std::string* out, uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                out->push_back(static_cast<char>((value >> shift) & 0xFF));
        }
        void PutU64LE(std::string* out, uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8)
                out->push_back(static_cast<char>((value >> shift) & 0xFF));
        }
        uint32_t GetU32LE(const char* data) {
            uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << (8 * i);
            return value;
        }
        uint64_t GetU64LE(const char* data) {
            uint64_t value = 0;
            for (unsigned i = 0; i < 8; ++i)
                value |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (8 * i);
            return value;
        }
    }

    // ============================================================================
    // WAL
    //
    // ★ 打开用 NewAppendableFile（不截断），Reset 用 NewWritableFile（截断）
    // ============================================================================

    WAL::WAL(std::string path, bool sync, uint64_t initial_sequence)
        : WAL(std::move(path), sync, initial_sequence, false) {
    }

    WAL::WAL(std::string path, bool sync, uint64_t initial_sequence,
        bool collect_write_path_stats)
        : path_(std::move(path)), sync_(sync),
        collect_write_path_stats_(collect_write_path_stats),
        next_sequence_(initial_sequence) {
        open_status_ = FileSystem::Default()->NewAppendableFile(path_, &file_);
        if (!open_status_.ok()) last_error_ = open_status_;
    }

    WAL::~WAL() = default;

    Status WAL::Append(const std::string& key, const std::string& value,
        ValueType type, uint64_t* sequence) {
        if (sequence) *sequence = 0;
        if (key.size() > (std::numeric_limits<uint32_t>::max)() ||
            value.size() > (std::numeric_limits<uint32_t>::max)())
            return Status::InvalidArgument("WAL key/value is too large");
        const uint64_t payload_size = 19ull + key.size() + value.size();
        if (payload_size >= kWalV2Flag)
            return Status::InvalidArgument("WAL record is too large");

        const auto lock_started = collect_write_path_stats_
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::unique_lock lk(mu_);
        if (collect_write_path_stats_)
            mutex_wait_ns_.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - lock_started).count()),
                std::memory_order_relaxed);
        if (!file_) return open_status_.ok() ? Status::IOError("WAL not open") : open_status_;
        if (!last_error_.ok()) return last_error_;
        const uint64_t assigned_sequence = next_sequence_++;

        std::string payload;
        payload.reserve(static_cast<size_t>(payload_size));
        payload.push_back(static_cast<char>(kWalV2Marker));
        payload.push_back(static_cast<char>(kWalV2Version));
        payload.push_back(static_cast<char>(type));
        PutU64LE(&payload, assigned_sequence);
        PutU32LE(&payload, static_cast<uint32_t>(key.size()));
        PutU32LE(&payload, static_cast<uint32_t>(value.size()));
        payload.append(key);
        payload.append(value);

        uint32_t crc = CRC32C::Compute(payload.data(), payload.size());
        uint32_t length = static_cast<uint32_t>(payload.size()) | kWalV2Flag;
        std::string record;
        record.reserve(8 + payload.size());
        record.append(reinterpret_cast<const char*>(&length), sizeof(length));
        record.append(reinterpret_cast<const char*>(&crc), sizeof(crc));
        record.append(payload);
        if (sequence) *sequence = assigned_sequence;

        uint64_t my_ticket = next_ticket_++;
        pending_.push_back({ std::move(record), my_ticket });

        while (true) {
            if (my_ticket < flushed_ticket_) return Status::OK();
            if (my_ticket < failed_ticket_)  return last_error_;

            if (!leader_active_) {
                if (pending_.empty()) {
                    cv_.notify_all();
                    return Status::IOError("WAL group commit queue is unexpectedly empty");
                }
                leader_active_ = true;
                std::vector<PendingWrite> batch = std::move(pending_);
                pending_.clear();
                uint64_t batch_end = batch.back().ticket + 1;
                if (collect_write_path_stats_) {
                    batches_.fetch_add(1, std::memory_order_relaxed);
                    records_.fetch_add(batch.size(), std::memory_order_relaxed);
                }

                lk.unlock();
                Status st = FlushBatch(batch);
                lk.lock();

                leader_active_ = false;

                if (st.ok()) {
                    if (batch_end > flushed_ticket_) flushed_ticket_ = batch_end;
                }
                else {
                    if (batch_end > failed_ticket_) failed_ticket_ = batch_end;
                    last_error_ = st;
                }
                cv_.notify_all();

                if (my_ticket < flushed_ticket_) return Status::OK();
                if (my_ticket < failed_ticket_)  return last_error_;
                continue;
            }

            const auto wait_started = collect_write_path_stats_
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            cv_.wait(lk);
            if (collect_write_path_stats_)
                queue_wait_ns_.fetch_add(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - wait_started).count()),
                    std::memory_order_relaxed);
        }
    }

    Status WAL::FlushBatch(const std::vector<PendingWrite>& batch) {
        size_t total = 0;
        for (auto& p : batch) total += p.record.size();
        std::string buf;
        buf.reserve(total);
        for (auto& p : batch) buf.append(p.record);

        const auto append_started = collect_write_path_stats_
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Status st = file_->Append(Slice(buf));
        if (collect_write_path_stats_)
            append_io_ns_.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - append_started).count()),
                std::memory_order_relaxed);
        if (!st.ok()) return st;

        bytes_.fetch_add(buf.size(), std::memory_order_relaxed);

        if (sync_) {
            const auto sync_started = collect_write_path_stats_
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            st = file_->Sync();
            if (collect_write_path_stats_)
                sync_ns_.fetch_add(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - sync_started).count()),
                    std::memory_order_relaxed);
            if (!st.ok()) return st;
        }
        return Status::OK();
    }

    WAL::WriteStats WAL::GetWriteStats() const noexcept {
        WriteStats stats;
        stats.mutex_wait_ns = mutex_wait_ns_.load(std::memory_order_relaxed);
        stats.queue_wait_ns = queue_wait_ns_.load(std::memory_order_relaxed);
        stats.append_io_ns = append_io_ns_.load(std::memory_order_relaxed);
        stats.sync_ns = sync_ns_.load(std::memory_order_relaxed);
        stats.batches = batches_.load(std::memory_order_relaxed);
        stats.records = records_.load(std::memory_order_relaxed);
        return stats;
    }

    Status WAL::Recover(uint64_t last_sequence, std::vector<Record>* records) {
        if (!records) return Status::InvalidArgument("null WAL recovery output");
        std::lock_guard lk(mu_);
        records->clear();

        std::unique_ptr<RandomAccessFile> rf;
        Status st = FileSystem::Default()->NewRandomAccessFile(path_, &rf);
        if (!st.ok()) return st;

        const uint64_t file_size = rf->Size();
        if (file_size > 1024ull * 1024 * 1024)
            return Status::Corruption("WAL file exceeds recovery limit");
        std::string buf(static_cast<size_t>(file_size), '\0');
        if (file_size != 0) {
            Slice result;
            st = rf->Read(0, file_size, &result, buf.data());
            if (!st.ok()) return st;
            if (result.size() != file_size)
                return Status::IOError("short read from WAL");
        }

        size_t p = 0;
        uint64_t sequence_cursor = last_sequence;
        uint64_t last_record_sequence = 0;
        while (p < buf.size()) {
            const size_t remaining = buf.size() - p;
            if (remaining < 8) break;
            uint32_t header = 0, crc = 0;
            std::memcpy(&header, buf.data() + p, sizeof(header));
            std::memcpy(&crc, buf.data() + p + sizeof(header), sizeof(crc));
            p += 8;
            const bool versioned = (header & kWalV2Flag) != 0;
            const uint32_t length = versioned ? (header & ~kWalV2Flag) : header;
            if (length == 0) return Status::Corruption("empty WAL record");
            if (length > buf.size() - p) { p -= 8; break; }
            const char* payload = buf.data() + p;
            if (CRC32C::Compute(payload, length) != crc)
                return Status::Corruption("WAL checksum mismatch");

            Record record;
            if (versioned) {
                constexpr size_t kHeaderSize = 19;
                if (length < kHeaderSize ||
                    static_cast<uint8_t>(payload[0]) != kWalV2Marker ||
                    static_cast<uint8_t>(payload[1]) != kWalV2Version)
                    return Status::Corruption("invalid WAL v2 record header");
                const uint8_t type = static_cast<uint8_t>(payload[2]);
                if (type > static_cast<uint8_t>(ValueType::kValue))
                    return Status::Corruption("invalid WAL value type");
                record.type = static_cast<ValueType>(type);
                record.sequence = GetU64LE(payload + 3);
                const uint32_t key_length = GetU32LE(payload + 11);
                const uint32_t value_length = GetU32LE(payload + 15);
                if (static_cast<uint64_t>(kHeaderSize) + key_length + value_length != length)
                    return Status::Corruption("invalid WAL key/value lengths");
                if (last_record_sequence != 0 && record.sequence <= last_record_sequence)
                    return Status::Corruption("WAL sequence is not increasing");
                last_record_sequence = record.sequence;
                sequence_cursor = std::max(sequence_cursor, record.sequence);
                record.key.assign(payload + kHeaderSize, key_length);
                record.value.assign(payload + kHeaderSize + key_length, value_length);
            } else {
                if (length < 9) return Status::Corruption("invalid legacy WAL record");
                const uint8_t type = static_cast<uint8_t>(payload[0]);
                if (type > static_cast<uint8_t>(ValueType::kValue))
                    return Status::Corruption("invalid legacy WAL value type");
                uint32_t key_length = 0, value_length = 0;
                std::memcpy(&key_length, payload + 1, sizeof(key_length));
                std::memcpy(&value_length, payload + 5, sizeof(value_length));
                if (9ull + key_length + value_length != length)
                    return Status::Corruption("invalid legacy WAL key/value lengths");
                if (sequence_cursor == (std::numeric_limits<uint64_t>::max)())
                    return Status::Corruption("WAL sequence space exhausted");
                record.sequence = ++sequence_cursor;
                last_record_sequence = record.sequence;
                record.type = static_cast<ValueType>(type);
                record.key.assign(payload + 9, key_length);
                record.value.assign(payload + 9 + key_length, value_length);
            }
            records->push_back(std::move(record));
            p += length;
        }

        if (p < buf.size()) {
            rf.reset();
            file_.reset();
            st = FileSystem::Default()->TruncateFile(path_, p);
            if (!st.ok()) return st;
            open_status_ = FileSystem::Default()->NewAppendableFile(path_, &file_);
            if (!open_status_.ok()) {
                last_error_ = open_status_;
                return open_status_;
            }
        }
        if (sequence_cursor == (std::numeric_limits<uint64_t>::max)())
            return Status::Corruption("WAL sequence space exhausted");
        next_sequence_ = std::max(next_sequence_, sequence_cursor + 1);
        return Status::OK();
    }

    std::vector<WAL::Record> WAL::Recover() {
        std::vector<Record> records;
        (void)Recover(0, &records);
        return records;
    }

    void WAL::SetNextSequence(uint64_t sequence) {
        std::lock_guard lk(mu_);
        next_sequence_ = std::max(next_sequence_, sequence);
    }

    Status WAL::Reset() {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [this] {
            return !leader_active_ && pending_.empty();
        });

        file_.reset();
        std::unique_ptr<WritableFile> replacement;
        Status st = FileSystem::Default()->NewWritableFile(path_, &replacement);
        if (!st.ok()) {
            last_error_ = st;
            return st;
        }

        file_ = std::move(replacement);
        bytes_.store(0, std::memory_order_relaxed);
        return Status::OK();
    }

} // namespace lsm