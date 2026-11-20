#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "lsm/memtable.h"
#include "lsm/sstable.h"
#include "lsm/version_set.h"

namespace lsm {

    struct SuperVersion {
        std::shared_ptr<MemTable> mutable_mem;
        std::vector<std::shared_ptr<MemTable>> immutable_mems;
        std::shared_ptr<const Version> version;

        mutable std::atomic<int> refs{ 0 };
        void Ref() const { refs.fetch_add(1, std::memory_order_relaxed); }
        bool Unref() const {
            return refs.fetch_sub(1, std::memory_order_acq_rel) == 1;
        }
    };

    // ============================================================================
    // VersionManager：受互斥锁保护的读视图获取与延迟释放
    //
    // current_sv_ 的读取和 Ref 必须在同一临界区中完成，避免 Install/PurgeObsolete
    // 在指针取得后、引用增加前释放对象。读者用完视图后调用 Unref。
    // ============================================================================
    class VersionManager {
    public:
        VersionManager();
        ~VersionManager();

        SuperVersion* GetReadView();
        void Install(SuperVersion* new_sv);
        void PurgeObsolete();

        uint64_t CurrentVersionNumber() const {
            return current_version_number_.load(std::memory_order_acquire);
        }

    private:
        std::mutex mu_;
        SuperVersion* current_sv_ = nullptr;
        std::atomic<uint64_t> current_version_number_{ 0 };
        std::vector<SuperVersion*> pending_free_;
    };

} // namespace lsm