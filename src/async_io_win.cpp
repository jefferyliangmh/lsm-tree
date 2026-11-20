#include "lsm/async_io.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace lsm {

    namespace {

        struct OverlappedContext {
            OVERLAPPED ov{};
            HANDLE event = nullptr;
            uint64_t user_data = 0;
            DWORD expected_bytes = 0;
            DWORD immediate_error = ERROR_SUCCESS;
        };

        class WinOverlappedAsyncIO : public AsyncIOBackend {
        public:
            ~WinOverlappedAsyncIO() override { CloseEvents(); }

            Status Submit(const std::vector<Request>& reqs) override {
                if (active_)
                    return Status(Code::kBusy, "overlapped batch already submitted");

                contexts_.assign(reqs.size(), {});
                requests_.resize(reqs.size(), nullptr);
                active_ = true;

                for (size_t i = 0; i < reqs.size(); ++i) {
                    const auto& r = reqs[i];
                    requests_[i] = r.file ? r.file->NativeHandle() : nullptr;
                    auto& ctx = contexts_[i];
                    ctx.user_data = r.user_data;

                    if (!r.file || !r.buf || r.len == 0 || r.len > MAXDWORD) {
                        ctx.immediate_error = ERROR_INVALID_PARAMETER;
                        continue;
                    }

                    HANDLE h = reinterpret_cast<HANDLE>(r.file->NativeHandle());
                    if (!h || h == INVALID_HANDLE_VALUE) {
                        ctx.immediate_error = ERROR_INVALID_HANDLE;
                        continue;
                    }

                    ctx.event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (!ctx.event) {
                        ctx.immediate_error = ::GetLastError();
                        continue;
                    }
                    ctx.ov.hEvent = ctx.event;
                    ctx.expected_bytes = static_cast<DWORD>(r.len);
                    ctx.ov.Offset = static_cast<DWORD>(r.offset);
                    ctx.ov.OffsetHigh = static_cast<DWORD>(r.offset >> 32);

                    DWORD bytes = 0;
                    BOOL ok = ::ReadFile(h, r.buf, ctx.expected_bytes, &bytes, &ctx.ov);
                    if (!ok) {
                        DWORD err = ::GetLastError();
                        if (err != ERROR_IO_PENDING) ctx.immediate_error = err;
                    }
                }
                return Status::OK();
            }

            Status Wait(unsigned n,
                std::vector<uint64_t>* completed,
                std::vector<Status>* statuses) override {
                completed->clear();
                statuses->clear();
                if (!active_ || n != contexts_.size())
                    return Status::InvalidArgument(
                        "overlapped wait does not match submitted batch");

                completed->reserve(contexts_.size());
                statuses->reserve(contexts_.size());
                for (size_t i = 0; i < contexts_.size(); ++i) {
                    auto& ctx = contexts_[i];
                    completed->push_back(ctx.user_data);

                    if (ctx.immediate_error != ERROR_SUCCESS) {
                        statuses->push_back(MakeIOError(
                            "async read failed", ctx.immediate_error));
                        continue;
                    }

                    HANDLE h = reinterpret_cast<HANDLE>(requests_[i]);
                    DWORD bytes = 0;
                    if (!::GetOverlappedResult(h, &ctx.ov, &bytes, TRUE)) {
                        statuses->push_back(MakeIOError(
                            "async read failed", ::GetLastError()));
                    }
                    else if (bytes != ctx.expected_bytes) {
                        statuses->push_back(Status::IOError("short asynchronous read"));
                    }
                    else {
                        statuses->push_back(Status::OK());
                    }
                }

                CloseEvents();
                requests_.clear();
                active_ = false;
                return Status::OK();
            }

        private:
            static std::string MakeErrorMessage(const char* prefix, DWORD error) {
                char buf[128] = {};
                std::snprintf(buf, sizeof(buf), "%s: %lu", prefix, error);
                return buf;
            }

            static Status MakeIOError(const char* prefix, DWORD error) {
                return Status::IOError(MakeErrorMessage(prefix, error));
            }

            void CloseEvents() {
                for (auto& ctx : contexts_) {
                    if (ctx.event) {
                        ::CloseHandle(ctx.event);
                        ctx.event = nullptr;
                    }
                }
                contexts_.clear();
            }

            std::vector<OverlappedContext> contexts_;
            std::vector<void*> requests_;
            bool active_ = false;
        };

    } // namespace

    AsyncIOBackend* AsyncIOBackend::Default() {
        static thread_local WinOverlappedAsyncIO backend;
        return &backend;
    }

} // namespace lsm