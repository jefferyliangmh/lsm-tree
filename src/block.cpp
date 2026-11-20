#include "lsm/block.h"

#include <cstring>
#include "lsm/internal_key.h"

namespace lsm {

    // ============================================================================
    // varint32
    // ============================================================================

    void PutVarint32(std::string* dst, uint32_t value) {
        while (value >= 0x80) {
            dst->push_back(static_cast<char>(value | 0x80));
            value >>= 7;
        }
        dst->push_back(static_cast<char>(value));
    }

    bool GetVarint32(std::string_view* input, uint32_t* value) {
        uint32_t result = 0;
        for (int shift = 0; shift <= 28 && !input->empty(); shift += 7) {
            uint8_t byte = static_cast<uint8_t>((*input)[0]);
            input->remove_prefix(1);
            result |= static_cast<uint32_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                *value = result;
                return true;
            }
        }
        return false;
    }

    // ============================================================================
    // BlockBuilder
    // ============================================================================

    void BlockBuilder::Add(const Slice& key, const Slice& value) {
        size_t shared = 0;
        if (counter_ < restart_interval_) {
            size_t min_len = std::min(last_key_.size(), key.size());
            while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
        }
        else {
            AddRestartPoint();
        }

        size_t non_shared = key.size() - shared;
        PutVarint32(&buffer_, static_cast<uint32_t>(shared));
        PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
        PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
        buffer_.append(key.data() + shared, non_shared);
        buffer_.append(value.data(), value.size());

        last_key_.assign(key.data(), key.size());
        ++counter_;
    }

    void BlockBuilder::AddRestartPoint() {
        restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
        counter_ = 0;
    }

    Slice BlockBuilder::Finish() {
        for (uint32_t r : restarts_) {
            buffer_.append(reinterpret_cast<const char*>(&r), 4);
        }
        uint32_t num = static_cast<uint32_t>(restarts_.size());
        buffer_.append(reinterpret_cast<const char*>(&num), 4);
        finished_ = true;
        return Slice(buffer_);
    }

    size_t BlockBuilder::CurrentSizeEstimate() const {
        return buffer_.size() + restarts_.size() * 4 + 4;
    }

    void BlockBuilder::Reset() {
        buffer_.clear();
        restarts_.clear();
        restarts_.push_back(0);
        counter_ = 0;
        last_key_.clear();
        finished_ = false;
    }

    // ============================================================================
    // Block
    // ============================================================================

    Block::Block(std::string data) : data_(std::move(data)) {
        if (data_.size() < 4) {
            num_restarts_ = 0;
            return;
        }
        std::memcpy(&num_restarts_, data_.data() + data_.size() - 4, 4);
        if (num_restarts_ == 0 ||
            num_restarts_ > (data_.size() - 4) / 4) {
            num_restarts_ = 0;
            return;
        }
        restarts_offset_ = static_cast<uint32_t>(
            data_.size() - 4 - num_restarts_ * 4);
    }

    // ★ 关键改动：一次解析返回所有需要的信息，消除重复解析
    //   返回 false 时所有输出参数都不写，调用者必须在 false 时提前返回
    bool Block::DecodeEntry(uint32_t offset, uint32_t* shared,
        uint32_t* non_shared, uint32_t* value_len,
        uint32_t* key_offset, uint32_t* next_offset) const {
        if (offset >= restarts_offset_) return false;

        std::string_view input(data_.data() + offset, restarts_offset_ - offset);

        uint32_t s = 0, ns = 0, vl = 0;
        if (!GetVarint32(&input, &s))  return false;
        if (!GetVarint32(&input, &ns)) return false;
        if (!GetVarint32(&input, &vl)) return false;

        // header 长度 = 三个 varint 的总字节数
        size_t header_len = static_cast<size_t>(
            input.data() - (data_.data() + offset));

        // 计算 entry 结束位置，检查边界
        uint64_t end = static_cast<uint64_t>(offset) + header_len + ns + vl;
        if (end > restarts_offset_) return false;

        *shared = s;
        *non_shared = ns;
        *value_len = vl;
        *key_offset = offset + static_cast<uint32_t>(header_len);
        *next_offset = static_cast<uint32_t>(end);
        return true;
    }

    bool Block::Seek(const Slice& target, std::string* key, Slice* value) const {
        if (num_restarts_ == 0) return false;

        // ========================================================================
        // 第 1 步：restart 数组二分（在 restart 点上）
        //
        // 这里对 mid_key 调用 CompareInternal，会自动走 SIMD 加速的
        // CompareUserKeys 路径（key ≥ 16 字节时一次比较 16 字节）。
        // ========================================================================
        uint32_t left = 0, right = num_restarts_ - 1;
        while (left < right) {
            uint32_t mid = (left + right + 1) / 2;
            uint32_t region_offset = 0;
            std::memcpy(&region_offset,
                data_.data() + restarts_offset_ + mid * 4, 4);

            uint32_t s = 0, ns = 0, vl = 0, koff = 0, noff = 0;
            if (!DecodeEntry(region_offset, &s, &ns, &vl, &koff, &noff))
                return false;

            Slice mid_key(data_.data() + koff, ns);
            if (CompareInternal(mid_key, target) < 0) left = mid;
            else right = mid - 1;
        }

        // ========================================================================
        // 第 2 步：从 restart 起点线性扫描
        //
        // 优化点：
        //   - prev_key / current_key 都预留 capacity，避免每次迭代重新分配
        //   - resize + memcpy + append 比 assign + append 更快（复用已有 buffer）
        //   - swap(prev_key, current_key) 避免 std::string 拷贝
        //   - CompareInternal 内部走 SIMD 加速的 user_key 比较
        // ========================================================================
        uint32_t offset = 0;
        std::memcpy(&offset, data_.data() + restarts_offset_ + left * 4, 4);

        std::string prev_key;
        std::string current_key;
        prev_key.reserve(64);
        current_key.reserve(64);

        while (offset < restarts_offset_) {
            uint32_t s = 0, ns = 0, vl = 0, koff = 0, noff = 0;
            if (!DecodeEntry(offset, &s, &ns, &vl, &koff, &noff)) return false;

            // 复用 current_key 的 buffer（resize 不释放已有 capacity）
            current_key.resize(s);
            if (s > 0) std::memcpy(&current_key[0], prev_key.data(), s);
            current_key.append(data_.data() + koff, ns);

            if (current_key.size() < 8) return false;

            Slice current_key_slice(current_key);
            if (CompareInternal(current_key_slice, target) >= 0) {
                *key = std::move(current_key);
                *value = Slice(data_.data() + koff + ns, vl);
                return true;
            }

            prev_key.swap(current_key);
            offset = noff;
        }
        return false;
    }

} // namespace lsm