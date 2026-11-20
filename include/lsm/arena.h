#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace lsm {

// Arena 分配器：
//   - 4KB 块预分配，小对象从块内线性切分，避免逐节点 malloc
//   - 大对象（> kBlockSize/4）独立 malloc，不浪费块空间
//   - Reset() 批量释放，O(块数) 回收整个 MemTable
//   - 非线程安全：由调用者保证（MemTable 单写者）
class Arena {
public:
    static constexpr size_t kBlockSize = 4096;

    Arena() = default;
    ~Arena() { Reset(); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    char* Allocate(size_t bytes) {
        if (bytes == 0) return nullptr;
        if (bytes <= alloc_bytes_remaining_) {
            char* result = alloc_ptr_;
            alloc_ptr_ += bytes;
            alloc_bytes_remaining_ -= bytes;
            return result;
        }
        return AllocateFallback(bytes);
    }

    char* AllocateAligned(size_t bytes, size_t align = alignof(std::max_align_t)) {
        char* ptr = Allocate(bytes + align);
        if (!ptr) return nullptr;
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t aligned = (addr + align - 1) & ~(align - 1);
        return reinterpret_cast<char*>(aligned);
    }

    size_t MemoryUsage() const {
        size_t total = blocks_.size() * kBlockSize;
        total += large_bytes_;
        return total;
    }

    void Reset() {
        for (auto* b : blocks_) std::free(b);
        blocks_.clear();
        for (auto* b : large_allocations_) std::free(b);
        large_allocations_.clear();
        alloc_ptr_ = nullptr;
        alloc_bytes_remaining_ = 0;
        large_bytes_ = 0;
    }

private:
    char* AllocateFallback(size_t bytes) {
        if (bytes > kBlockSize / 4) {
            char* result = static_cast<char*>(std::malloc(bytes));
            if (!result) std::abort();
            large_allocations_.push_back(result);
            large_bytes_ += bytes;
            return result;
        }
        char* block = static_cast<char*>(std::malloc(kBlockSize));
        if (!block) std::abort();
        blocks_.push_back(block);
        alloc_ptr_ = block + bytes;
        alloc_bytes_remaining_ = kBlockSize - bytes;
        return block;
    }

    std::vector<char*> blocks_;
    std::vector<char*> large_allocations_;
    char*  alloc_ptr_ = nullptr;
    size_t alloc_bytes_remaining_ = 0;
    size_t large_bytes_ = 0;
};

} // namespace lsm
