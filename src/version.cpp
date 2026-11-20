#include "lsm/version.h"

namespace lsm {

    VersionManager::VersionManager() {
        current_sv_ = new SuperVersion();
        current_sv_->Ref();   // owner 持有 1 个引用
        current_version_number_.store(1, std::memory_order_release);
    }

    VersionManager::~VersionManager() {
        // 调用方必须保证所有线程已停止访问 DB
        delete current_sv_;
        for (auto* sv : pending_free_) delete sv;
    }

    SuperVersion* VersionManager::GetReadView() {
        std::lock_guard lk(mu_);
        SuperVersion* sv = current_sv_;
        sv->Ref();
        return sv;
    }

    void VersionManager::Install(SuperVersion* new_sv) {
        std::lock_guard lk(mu_);
        SuperVersion* old = current_sv_;
        new_sv->Ref();           // owner 持有
        current_sv_ = new_sv;
        current_version_number_.fetch_add(1, std::memory_order_release);

        old->Unref();            // owner 释放
        // 调用方可能仍持有旧视图，延迟到引用计数归零后再释放。
        pending_free_.push_back(old);
    }

    void VersionManager::PurgeObsolete() {
        std::lock_guard lk(mu_);
        auto it = pending_free_.begin();
        while (it != pending_free_.end()) {
            SuperVersion* sv = *it;
            if (sv == current_sv_) { ++it; continue; }
            if (sv->refs.load(std::memory_order_acquire) == 0) {
                delete sv;
                it = pending_free_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

} // namespace lsm