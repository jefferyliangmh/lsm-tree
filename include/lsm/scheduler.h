#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace lsm {

    // ============================================================================
    // 并发模型（flush / compaction 流水线）
    //
    // 两个互不共享队列的线程池：
    //   - flush 池：消化 immutable memtable。任务之间再用 flush_mu_ 串行，
    //     保证 WAL 代际按从旧到新刷盘。默认 1 个线程。
    //   - compaction 池：执行归并。同一 DB 同时只允许一个 compaction
    //     （compaction_mu_），避免两份任务选中同一批 SST。
    //
    // 因为池是分开的，flush 写新 SST 与 compaction 的异步预取/归并可以重叠，
    // 前台 Put 只受 immutable 队列和 compaction backlog 背压，不跟归并共用一把大锁。
    //
    // 背压：
    //   - immutable_mems >= immutable_slowdown_threshold：写路径短睡
    //   - immutable_mems >= immutable_stop_threshold：写路径等待 flush
    //   - compaction 排队 + 在途 >= max_compaction_backlog：写路径再短睡
    //
    // 队列无界，Submit 不阻塞，因此持有 compaction_mu_ 的线程不会反过来等队列。
    // Shutdown 后 Submit 返回 false，已入队任务仍会执行完。
    // ============================================================================
    class ThreadPool {
    public:
        ThreadPool(int threads, std::string name);
        ~ThreadPool();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        bool Submit(std::function<void()> fn);
        void Shutdown();

        size_t Queued() const;
        uint64_t Completed() const noexcept {
            return completed_.load(std::memory_order_relaxed);
        }

    private:
        void Worker();

        std::string name_;
        mutable std::mutex mu_;
        std::condition_variable cv_;
        std::queue<std::function<void()>> queue_;
        std::vector<std::thread> threads_;
        bool stopping_ = false;
        std::atomic<uint64_t> completed_{ 0 };
    };

    class BackgroundScheduler {
    public:
        BackgroundScheduler(int flush_threads, int compaction_threads);
        ~BackgroundScheduler();

        BackgroundScheduler(const BackgroundScheduler&) = delete;
        BackgroundScheduler& operator=(const BackgroundScheduler&) = delete;

        bool ScheduleFlush(std::function<void()> fn);
        bool ScheduleCompaction(std::function<void()> fn);
        void Shutdown();

        size_t FlushQueued() const { return flush_.Queued(); }
        size_t CompactionQueued() const { return compaction_.Queued(); }
        uint64_t FlushCompleted() const noexcept { return flush_.Completed(); }
        uint64_t CompactionCompleted() const noexcept { return compaction_.Completed(); }

    private:
        ThreadPool flush_;
        ThreadPool compaction_;
    };

} // namespace lsm
