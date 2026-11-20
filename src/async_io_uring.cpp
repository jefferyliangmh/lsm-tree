// ============================================================================
// AsyncIOBackend 的 Linux 实现
//
// 主路径：io_uring（需要 liburing + 内核 5.10+）
// 回退：线程池并行 pread（io_uring 被云厂商禁用或初始化失败时）
//
// io_uring 被禁用的常见原因：
//   - /proc/sys/kernel/io_uring_disabled == 2（部分云实例默认配置）
//   - 旧内核（< 5.1）
//   - 容器权限限制
//
// 即使走线程池 fallback，15 个 SST 的并行 pread 仍比串行快 3-5×。
// ============================================================================

#include "lsm/async_io.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef HAS_LIBURING
#include <liburing.h>
#endif

namespace lsm {

    namespace {

        class UringAsyncIO : public AsyncIOBackend {
        public:
            UringAsyncIO() = default;

            ~UringAsyncIO() override {
#ifdef HAS_LIBURING
                if (ring_inited_) io_uring_queue_exit(&ring_);
#endif
            }

            Status Submit(const std::vector<Request>& reqs) override {
                if (reqs.empty()) return Status::OK();

#ifdef HAS_LIBURING
                // 首次 Submit 时初始化 io_uring；失败则永久标记不可用
                if (!ring_inited_ && !uring_failed_) {
                    int ret = io_uring_queue_init(64, &ring_, 0);
                    if (ret < 0) {
                        std::fprintf(stderr,
                            "[io_uring] queue_init failed: %s, "
                            "using thread-pool fallback\n",
                            std::strerror(-ret));
                        uring_failed_ = true;
                    }
                    else {
                        std::fprintf(stderr, "[io_uring] initialized successfully\n");
                        ring_inited_ = true;
                    }
                }

                if (ring_inited_) {
                    for (auto& r : reqs) {
                        auto* sqe = io_uring_get_sqe(&ring_);
                        if (!sqe) {
                            int ret = io_uring_submit(&ring_);
                            if (ret < 0)
                                return Status::IOError("io_uring_submit failed");
                            sqe = io_uring_get_sqe(&ring_);
                            if (!sqe) return Status::IOError("SQ ring full");
                        }
                        int fd = static_cast<int>(
                            reinterpret_cast<intptr_t>(r.file->NativeHandle()));
                        io_uring_prep_read(sqe, fd, r.buf,
                            static_cast<unsigned>(r.len), r.offset);
                        sqe->user_data = r.user_data;
                    }

                    int ret = io_uring_submit(&ring_);
                    if (ret < 0)
                        return Status::IOError(std::string("io_uring_submit: ") +
                            std::strerror(-ret));

                    mode_ = Mode::kUring;
                    return Status::OK();
                }
#endif

                // 回退：线程池并行 pread
                ThreadPoolFallback(reqs);
                mode_ = Mode::kFallback;
                return Status::OK();
            }

            Status Wait(unsigned n,
                std::vector<uint64_t>* completed,
                std::vector<Status>* statuses) override {
                completed->clear();
                statuses->clear();

                if (mode_ == Mode::kUring) {
#ifdef HAS_LIBURING
                    unsigned waited = 0;
                    while (waited < n) {
                        struct io_uring_cqe* cqe = nullptr;
                        int ret = io_uring_wait_cqe(&ring_, &cqe);
                        if (ret < 0)
                            return Status::IOError(
                                std::string("io_uring_wait_cqe: ") + std::strerror(-ret));
                        if (cqe->res < 0) {
                            completed->push_back(cqe->user_data);
                            statuses->push_back(
                                Status::IOError(std::string("async read: ") +
                                    std::strerror(-cqe->res)));
                        }
                        else {
                            completed->push_back(cqe->user_data);
                            statuses->push_back(Status::OK());
                        }
                        io_uring_cqe_seen(&ring_, cqe);
                        ++waited;
                    }
#endif
                }
                else if (mode_ == Mode::kFallback) {
                    unsigned cnt = std::min<unsigned>(
                        n, static_cast<unsigned>(fallback_statuses_.size()));
                    for (unsigned i = 0; i < cnt; ++i) {
                        completed->push_back(fallback_tags_[i]);
                        statuses->push_back(fallback_statuses_[i]);
                    }
                }

                mode_ = Mode::kIdle;
                return Status::OK();
            }

        private:
            enum class Mode { kIdle, kUring, kFallback };

            // 线程池并行 pread：每个请求一个 std::thread
            // 对 OpenBatch 的 N 个请求，总耗时 ≈ 最慢的那个 pread
            void ThreadPoolFallback(const std::vector<Request>& reqs) {
                std::fprintf(stderr, "[io_uring] thread-pool fallback, %zu reqs\n",
                    reqs.size());

                fallback_tags_.assign(reqs.size(), 0);
                fallback_statuses_.assign(reqs.size(), Status::OK());

                std::vector<std::thread> threads;
                threads.reserve(reqs.size());
                for (size_t i = 0; i < reqs.size(); ++i) {
                    threads.emplace_back([this, &reqs, i]() {
                        Slice result;
                        Status st = reqs[i].file->Read(
                            reqs[i].offset, reqs[i].len, &result,
                            static_cast<char*>(reqs[i].buf));
                        fallback_tags_[i] = reqs[i].user_data;
                        fallback_statuses_[i] = st;
                        });
                }
                for (auto& t : threads) t.join();
            }

#ifdef HAS_LIBURING
            struct io_uring ring_ {};
            bool ring_inited_ = false;
            bool uring_failed_ = false;
#endif
            Mode                  mode_ = Mode::kIdle;
            std::vector<uint64_t> fallback_tags_;
            std::vector<Status>   fallback_statuses_;
        };

    } // namespace

    AsyncIOBackend* AsyncIOBackend::Default() {
        static UringAsyncIO backend;
        return &backend;
    }

} // namespace lsm