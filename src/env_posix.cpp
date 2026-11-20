// ============================================================================
// FileSystem 的 POSIX 实现。Linux + macOS 共用。
//
// 关键实现要点：
//   - PRead/PWrite 用 pread/pwrite（无 lseek，线程安全）
//   - Sync 用 fdatasync（Linux）或 fsync（macOS），只刷数据不刷元数据
//   - RenameFile 用 rename(2)，POSIX 保证原子性（同一文件系统内）
//   - SyncDir 用 open(O_DIRECTORY) + fsync，确保 rename 持久化
// ============================================================================
#include "lsm/env.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include "port/port.h"

namespace lsm {

namespace {

std::string ErrnoMsg(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

// ============================================================================
// PosixWritableFile
//
// 内部维护 offset_，Append 自动递增；AppendAt 用显式 offset。
// 析构时 close，不主动 fsync（由调用方决定何时 Sync）。
// ============================================================================
class PosixWritableFile : public WritableFile {
public:
    explicit PosixWritableFile(int fd) : fd_(fd) {
        struct stat st{};
        if (::fstat(fd_, &st) == 0) offset_ = st.st_size;
    }

    ~PosixWritableFile() override {
        if (fd_ >= 0) ::close(fd_);
    }

    Status Append(const Slice& data) override {
        return AppendAt(data, offset_);
    }

    Status AppendAt(const Slice& data, uint64_t offset) override {
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = ::pwrite(fd_, data.data() + written,
                                 data.size() - written,
                                 static_cast<off_t>(offset + written));
            if (n < 0) {
                if (errno == EINTR) continue;
                return Status::IOError(ErrnoMsg("pwrite"));
            }
            written += static_cast<size_t>(n);
        }
        if (offset + written > offset_) offset_ = offset + written;
        return Status::OK();
    }

    Status Flush() override {
        return Status::OK();   // pwrite 已经写入内核
    }

    Status Sync() override {
#ifdef __APPLE__
        if (::fsync(fd_) != 0) return Status::IOError(ErrnoMsg("fsync"));
#else
        if (::fdatasync(fd_) != 0) return Status::IOError(ErrnoMsg("fdatasync"));
#endif
        return Status::OK();
    }

    uint64_t Size() const override { return offset_; }

private:
    int      fd_;
    uint64_t offset_ = 0;
};

// ============================================================================
// PosixRandomAccessFile
//
// 每次 Read 用 pread，无共享偏移，天然线程安全。
// ============================================================================
class PosixRandomAccessFile : public RandomAccessFile {
public:
    PosixRandomAccessFile(int fd, uint64_t size) : fd_(fd), size_(size) {}

    ~PosixRandomAccessFile() override {
        if (fd_ >= 0) ::close(fd_);
    }

    Status Read(uint64_t offset, size_t n,
                Slice* result, char* scratch) const override {
        if (offset >= size_) {
            *result = Slice();
            return Status::OK();
        }
        size_t to_read = std::min<uint64_t>(n, size_ - offset);
        size_t done = 0;
        while (done < to_read) {
            ssize_t r = ::pread(fd_, scratch + done, to_read - done,
                                static_cast<off_t>(offset + done));
            if (r < 0) {
                if (errno == EINTR) continue;
                return Status::IOError(ErrnoMsg("pread"));
            }
            if (r == 0) break;   // EOF（不应发生，size_ 已知）
            done += static_cast<size_t>(r);
        }
        *result = Slice(scratch, done);
        return Status::OK();
    }

    uint64_t Size() const override { return size_; }

    void* NativeHandle() const override {
        return reinterpret_cast<void*>(static_cast<intptr_t>(fd_));
    }

private:
    int      fd_;
    uint64_t size_;
};

// ============================================================================
// PosixFileSystem
// ============================================================================
class PosixFileSystem : public FileSystem {
public:
    Status NewWritableFile(const std::string& path,
                           std::unique_ptr<WritableFile>* result) override {
        int fd = ::open(path.c_str(),
                        port::kOpenReadWriteCreate | port::kOpenTruncate,
                        port::kDefaultFileMode);
        if (fd < 0) {
            // 目录可能不存在：尝试 O_CREAT 一次不行就报错
            return Status::IOError(ErrnoMsg("open for write: " + path));
        }
        *result = std::make_unique<PosixWritableFile>(fd);
        return Status::OK();
    }

    Status NewRandomAccessFile(const std::string& path,
                               std::unique_ptr<RandomAccessFile>* result) override {
        int fd = ::open(path.c_str(), port::kOpenReadOnly);
        if (fd < 0) return Status::IOError(ErrnoMsg("open for read: " + path));
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return Status::IOError(ErrnoMsg("fstat: " + path));
        }
        *result = std::make_unique<PosixRandomAccessFile>(
            fd, static_cast<uint64_t>(st.st_size));
        return Status::OK();
    }

    Status FileExists(const std::string& path) override {
        return ::access(path.c_str(), F_OK) == 0
                   ? Status::OK()
                   : Status::NotFound(path);
    }

    Status GetFileSize(const std::string& path, uint64_t* size) override {
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0)
            return Status::IOError(ErrnoMsg("stat: " + path));
        *size = static_cast<uint64_t>(st.st_size);
        return Status::OK();
    }

    Status DeleteFile(const std::string& path) override {
        if (::unlink(path.c_str()) != 0)
            return Status::IOError(ErrnoMsg("unlink: " + path));
        return Status::OK();
    }

    Status TruncateFile(const std::string& path, uint64_t size) override {
        if (size > static_cast<uint64_t>((std::numeric_limits<off_t>::max)()))
            return Status::InvalidArgument("truncate size out of range");
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) return Status::IOError(ErrnoMsg("open for truncate: " + path));
        int result = ::ftruncate(fd, static_cast<off_t>(size));
        int saved_errno = errno;
        ::close(fd);
        if (result != 0) {
            errno = saved_errno;
            return Status::IOError(ErrnoMsg("ftruncate: " + path));
        }
        return Status::OK();
    }

    Status RenameFile(const std::string& src, const std::string& dst) override {
        // POSIX rename 对同一文件系统内的操作是原子的
        if (::rename(src.c_str(), dst.c_str()) != 0)
            return Status::IOError(ErrnoMsg("rename: " + src + " -> " + dst));
        return Status::OK();
    }

    Status CreateDirIfMissing(const std::string& path) override {
        if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
            return Status::IOError(ErrnoMsg("mkdir: " + path));
        return Status::OK();
    }

    Status GetChildren(const std::string& dir,
                       std::vector<std::string>* result) override {
        DIR* d = ::opendir(dir.c_str());
        if (!d) return Status::IOError(ErrnoMsg("opendir: " + dir));
        result->clear();
        struct dirent* entry;
        while ((entry = ::readdir(d)) != nullptr) {
            const char* name = entry->d_name;
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
                continue;
            result->push_back(name);
        }
        ::closedir(d);
        return Status::OK();
    }

    Status SyncDir(const std::string& dir) override {
        // Linux: open(O_DIRECTORY) + fsync，保证 rename/mkdir 持久化
        int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) return Status::IOError(ErrnoMsg("open dir: " + dir));
        int ret = ::fsync(fd);
        ::close(fd);
        if (ret != 0) return Status::IOError(ErrnoMsg("fsync dir: " + dir));
        return Status::OK();
    }

    Status NewAppendableFile(const std::string& path,
        std::unique_ptr<WritableFile>* result) override {
        int fd = ::open(path.c_str(),
            port::kOpenReadWriteCreate | port::kOpenAppend,
            port::kDefaultFileMode);
        if (fd < 0)
            return Status::IOError(ErrnoMsg("open for append: " + path));
        *result = std::make_unique<PosixWritableFile>(fd);
        return Status::OK();
    }
};

} // namespace

// ============================================================================
// Default()：全局单例，进程生命周期内不变
// ============================================================================
FileSystem* FileSystem::Default() {
    if (auto* override = DefaultForTesting()) return override;
    static PosixFileSystem fs;
    return &fs;
}

} // namespace lsm