#include "lsm/scheduler.h"

#include <utility>

namespace lsm {

    ThreadPool::ThreadPool(int threads, std::string name) : name_(std::move(name)) {
        if (threads < 1) threads = 1;
        threads_.reserve(static_cast<size_t>(threads));
        for (int i = 0; i < threads; ++i)
            threads_.emplace_back([this] { Worker(); });
    }

    ThreadPool::~ThreadPool() { Shutdown(); }

    bool ThreadPool::Submit(std::function<void()> fn) {
        {
            std::lock_guard lk(mu_);
            if (stopping_) return false;
            queue_.push(std::move(fn));
        }
        cv_.notify_one();
        return true;
    }

    void ThreadPool::Shutdown() {
        {
            std::lock_guard lk(mu_);
            if (stopping_) {
                // 仍可能有线程没 join（重复 Shutdown）。
            } else {
                stopping_ = true;
            }
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

    size_t ThreadPool::Queued() const {
        std::lock_guard lk(mu_);
        return queue_.size();
    }

    void ThreadPool::Worker() {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                fn = std::move(queue_.front());
                queue_.pop();
            }
            fn();
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    BackgroundScheduler::BackgroundScheduler(int flush_threads, int compaction_threads)
        : flush_(flush_threads, "lsm-flush"),
          compaction_(compaction_threads, "lsm-compact") {}

    BackgroundScheduler::~BackgroundScheduler() { Shutdown(); }

    bool BackgroundScheduler::ScheduleFlush(std::function<void()> fn) {
        return flush_.Submit(std::move(fn));
    }

    bool BackgroundScheduler::ScheduleCompaction(std::function<void()> fn) {
        return compaction_.Submit(std::move(fn));
    }

    void BackgroundScheduler::Shutdown() {
        flush_.Shutdown();
        compaction_.Shutdown();
    }

} // namespace lsm
