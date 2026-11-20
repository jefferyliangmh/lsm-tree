#pragma once
// ============================================================================
// 平台无关的文件系统与 I/O 抽象
//
// 设计原则：
//   - 主框架（DB、Compaction、VersionSet、WAL、SSTable）只依赖此接口
//   - 各平台实现文件（env_posix.cpp / env_win.cpp）互不污染
//   - 源码中零平台 #ifdef；平台判断只在 port/port.h 和 CMakeLists.txt
//
// 对标 LevelDB port/ 与 RocksDB Env/FileSystem 分离方案。
// ============================================================================
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

    // ============================================================================
    // WritableFile：顺序追加写。WAL 和 TableBuilder 用。
    //
    // 生命周期：NewWritableFile 成功后由调用方管理；析构时自动关闭 fd/HANDLE。
    // 语义：Append 追加到当前末尾；AppendAt 在指定 offset 写入（覆盖）。
    // ============================================================================
    class WritableFile {
    public:
        virtual ~WritableFile() = default;
        virtual Status Append(const Slice& data) = 0;
        virtual Status AppendAt(const Slice& data, uint64_t offset) = 0;
        virtual Status Flush() = 0;
        virtual Status Sync() = 0;              // fdatasync / FlushFileBuffers
        virtual uint64_t Size() const = 0;
    };

    // ============================================================================
    // RandomAccessFile：随机位置读。TableReader 和 WAL Recover 用。
    //
    // 语义：Read 从 offset 读最多 n 字节。如果读到文件末尾不足 n，
    //       result 指向实际读到的数据，Status 仍是 OK。
    // ============================================================================
    class RandomAccessFile {
    public:
        virtual ~RandomAccessFile() = default;
        virtual Status Read(uint64_t offset, size_t n,
            Slice* result, char* scratch) const = 0;
        virtual uint64_t Size() const = 0;

        // ★ 返回原生句柄：POSIX 是 fd，Windows 是 HANDLE。
        // 只被 AsyncIOBackend 的平台实现调用；主框架不直接调用。
        virtual void* NativeHandle() const = 0;
    };

    // ============================================================================
    // FileSystem：文件与目录操作。主框架的所有路径操作都走这里。
    //
    // RenameFile 必须是原子的（MANIFEST 切换依赖此语义）。
    // SyncDir 在 Linux 上是 fsync(dirfd)，Windows 上是 FlushFileBuffers(dir_handle)。
    // ============================================================================
    class FileSystem {
    public:
        virtual ~FileSystem() = default;

        virtual Status NewWritableFile(const std::string& path,
            std::unique_ptr<WritableFile>* result) = 0;
        virtual Status NewRandomAccessFile(const std::string& path,
            std::unique_ptr<RandomAccessFile>* result) = 0;

        virtual Status FileExists(const std::string& path) = 0;
        virtual Status GetFileSize(const std::string& path, uint64_t* size) = 0;
        virtual Status DeleteFile(const std::string& path) = 0;
        virtual Status TruncateFile(const std::string& path, uint64_t size) = 0;
        virtual Status RenameFile(const std::string& src,
            const std::string& dst) = 0;

        virtual Status CreateDirIfMissing(const std::string& path) = 0;
        virtual Status GetChildren(const std::string& dir,
            std::vector<std::string>* result) = 0;
        virtual Status SyncDir(const std::string& dir) = 0;

        // ★ 追加打开：不截断已有内容，用于 WAL 和 MANIFEST 追加
        virtual Status NewAppendableFile(const std::string& path,
            std::unique_ptr<WritableFile>* result) = 0;

        // 全局单例。平台实现各自提供。
        static FileSystem* Default();

        static FileSystem* SetDefaultForTesting(FileSystem* file_system) noexcept {
            return test_override_.exchange(file_system, std::memory_order_acq_rel);
        }

        static FileSystem* DefaultForTesting() noexcept {
            return test_override_.load(std::memory_order_acquire);
        }

    private:
        static inline std::atomic<FileSystem*> test_override_{ nullptr };
    };

} // namespace lsm