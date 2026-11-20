#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lsm/status.h"

#ifdef HAS_LIBURING
#include <liburing.h>
#endif

namespace lsm {

class DirectIO {
public:
    static int Open(const std::string& path, bool direct = false) {
        int flags = O_RDWR | O_CREAT;
        if (direct) flags |= O_DIRECT;
        int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0)
            throw std::runtime_error("open failed: " + path +
                                     " errno=" + std::to_string(errno));
        return fd;
    }

    static int OpenRead(const std::string& path) {
        return ::open(path.c_str(), O_RDONLY);
    }

    static ssize_t PRead(int fd, void* buf, size_t n, off_t off) {
        ssize_t r;
        do { r = ::pread(fd, buf, n, off); } while (r < 0 && errno == EINTR);
        return r;
    }

    static ssize_t PWrite(int fd, const void* buf, size_t n, off_t off) {
        ssize_t r;
        do { r = ::pwrite(fd, buf, n, off); } while (r < 0 && errno == EINTR);
        return r;
    }

    static void Close(int fd) noexcept {
        if (fd >= 0) ::close(fd);
    }
};

#ifdef HAS_LIBURING

class IoUring {
public:
    explicit IoUring(unsigned entries = 64) : entries_(entries) {
        int ret = io_uring_queue_init(entries, &ring_, 0);
        if (ret < 0)
            throw std::runtime_error(
                std::string("io_uring_queue_init: ") + std::strerror(-ret));
        valid_ = true;
    }

    ~IoUring() { if (valid_) io_uring_queue_exit(&ring_); }

    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;

    void PrepRead(int fd, void* buf, unsigned n, off_t off, uint64_t tag) {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) throw std::runtime_error("SQ ring full");
        io_uring_prep_read(sqe, fd, buf, n, off);
        sqe->user_data = tag;
    }

    void PrepWrite(int fd, const void* buf, unsigned n, off_t off, uint64_t tag) {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) throw std::runtime_error("SQ ring full");
        io_uring_prep_write(sqe, fd, buf, n, off);
        sqe->user_data = tag;
    }

    int Submit() {
        int n = io_uring_submit(&ring_);
        if (n < 0)
            throw std::runtime_error(
                std::string("io_uring_submit: ") + std::strerror(-n));
        return n;
    }

    struct Completion {
        int      result;
        uint64_t tag;
    };

    void WaitN(unsigned n, std::vector<Completion>& out) {
        out.clear();
        out.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            struct io_uring_cqe* cqe = nullptr;
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0)
                throw std::runtime_error(
                    std::string("io_uring_wait_cqe: ") + std::strerror(-ret));
            out.push_back({cqe->res, cqe->user_data});
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

private:
    struct io_uring ring_{};
    unsigned        entries_;
    bool            valid_ = false;
};

inline Status ParallelReadFiles(const std::vector<std::string>& paths,
                                std::vector<std::string>& contents) {
    contents.assign(paths.size(), {});
    if (paths.empty()) return Status::OK();

    std::vector<int> fds(paths.size(), -1);
    std::vector<size_t> sizes(paths.size(), 0);
    for (size_t i = 0; i < paths.size(); ++i) {
        fds[i] = ::open(paths[i].c_str(), O_RDONLY);
        if (fds[i] < 0) {
            for (auto fd : fds) DirectIO::Close(fd);
            return Status::IOError("open " + paths[i]);
        }
        struct stat st{};
        if (::fstat(fds[i], &st) != 0) {
            for (auto fd : fds) DirectIO::Close(fd);
            return Status::IOError("fstat " + paths[i]);
        }
        sizes[i] = static_cast<size_t>(st.st_size);
        contents[i].resize(sizes[i]);
    }

    size_t nreq = 0;
    for (size_t i = 0; i < sizes.size(); ++i)
        if (sizes[i] > 0) ++nreq;

    if (nreq > 0) {
        try {
            IoUring ring(static_cast<unsigned>(nreq + 4));
            for (size_t i = 0; i < paths.size(); ++i) {
                if (sizes[i] == 0) continue;
                ring.PrepRead(fds[i], contents[i].data(),
                              static_cast<unsigned>(sizes[i]), 0,
                              static_cast<uint64_t>(i));
            }
            ring.Submit();
            std::vector<IoUring::Completion> comps;
            ring.WaitN(static_cast<unsigned>(nreq), comps);
            for (auto& c : comps) {
                if (c.result < 0) {
                    for (auto fd : fds) DirectIO::Close(fd);
                    return Status::IOError(
                        std::string("io_uring read: ") + std::strerror(-c.result));
                }
            }
        } catch (const std::exception&) {
            for (size_t i = 0; i < paths.size(); ++i) {
                if (sizes[i] == 0) continue;
                size_t off = 0;
                while (off < sizes[i]) {
                    ssize_t r = DirectIO::PRead(fds[i], contents[i].data() + off,
                                                sizes[i] - off,
                                                static_cast<off_t>(off));
                    if (r <= 0) {
                        for (auto fd : fds) DirectIO::Close(fd);
                        return Status::IOError("pread fallback failed");
                    }
                    off += static_cast<size_t>(r);
                }
            }
        }
    }
    for (auto fd : fds) DirectIO::Close(fd);
    return Status::OK();
}

#else  // !HAS_LIBURING

inline Status ParallelReadFiles(const std::vector<std::string>& paths,
                                std::vector<std::string>& contents) {
    contents.assign(paths.size(), {});
    for (size_t i = 0; i < paths.size(); ++i) {
        int fd = ::open(paths[i].c_str(), O_RDONLY);
        if (fd < 0) return Status::IOError("open " + paths[i]);
        struct stat st{};
        if (::fstat(fd, &st) != 0) { ::close(fd); return Status::IOError("fstat"); }
        contents[i].resize(static_cast<size_t>(st.st_size));
        size_t off = 0;
        while (off < contents[i].size()) {
            ssize_t r = DirectIO::PRead(fd, contents[i].data() + off,
                                        contents[i].size() - off,
                                        static_cast<off_t>(off));
            if (r <= 0) { ::close(fd); return Status::IOError("pread"); }
            off += static_cast<size_t>(r);
        }
        ::close(fd);
    }
    return Status::OK();
}

#endif  // HAS_LIBURING

} // namespace lsm
