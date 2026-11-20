#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "lsm/env.h"
#include "lsm/status.h"

namespace lsm {

    // ============================================================================
    // AsyncIOBackend：批量异步读
    //
    // 平台实现：
    //   - Linux:   io_uring（src/async_io_uring.cpp）
    //   - Windows: overlapped I/O（src/async_io_win.cpp）
    //
    // 关键设计：
    //   - Request 是纯聚合类型（无用户构造函数），支持 Request{...} 花括号初始化
    //   - 用 RandomAccessFile* 而非 raw fd/HANDLE，主框架不感知平台
    //   - Submit 和 Wait 分开，让调用方可以 Submit 后做别的事再 Wait
    // ============================================================================
    class AsyncIOBackend {
    public:
        struct Request {
            RandomAccessFile* file = nullptr;
            uint64_t          offset = 0;
            void* buf = nullptr;
            size_t            len = 0;
            uint64_t          user_data = 0;
        };

        virtual ~AsyncIOBackend() = default;

        virtual Status Submit(const std::vector<Request>& reqs) = 0;

        virtual Status Wait(unsigned n,
            std::vector<uint64_t>* completed,
            std::vector<Status>* statuses) = 0;

        static AsyncIOBackend* Default();
    };

} // namespace lsm