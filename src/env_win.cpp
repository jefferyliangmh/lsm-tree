// ============================================================================
// FileSystem 的 Windows 实现
//
// 关键实现要点：
//   - CreateFileW + ReadFile/WriteFile + OVERLAPPED（等价 POSIX pread/pwrite）
//   - Sync 用 FlushFileBuffers
//   - RenameFile 用 MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
//   - SyncDir 用 FILE_FLAG_BACKUP_SEMANTICS + FlushFileBuffers
//
// ★ 命名空间布局（关键）：
//   1. lsm::port::{Utf8ToWide, WideToUtf8}   外部链接，供其他 TU 使用
//   2. lsm::(匿名)::{WinFileSystem, ...}     内部链接，仅本 TU 使用
//   如果把 port 命名空间嵌进匿名命名空间，Utf8ToWide 会变成内部链接，
//   导致链接其他 .exe 时报 LNK2019。
// ============================================================================

#include "lsm/env.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <windows.h>

// Windows.h 把 DeleteFile 定义为宏（→ DeleteFileA/W），
// 不 #undef 会让类方法 Status DeleteFile(...) 被宏替换，丢失 override
#ifdef DeleteFile
#undef DeleteFile
#endif

#include "port/port.h"

namespace lsm {

    // ============================================================================
    // UTF-8 ↔ UTF-16 转换：★ 必须放在匿名命名空间外，产生外部链接
    //
    // port_win.h 声明的是 lsm::port::Utf8ToWide（外部链接）。
    // 若定义在匿名命名空间内，链接器找不到该符号 → LNK2019。
    // ============================================================================
    namespace port {

        std::wstring Utf8ToWide(const std::string& utf8) {
            if (utf8.empty()) return {};
            int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                static_cast<int>(utf8.size()),
                nullptr, 0);
            if (wlen <= 0) return {};
            std::wstring wide(static_cast<size_t>(wlen), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                static_cast<int>(utf8.size()),
                wide.data(), wlen);
            return wide;
        }

        std::string WideToUtf8(const std::wstring& wide) {
            if (wide.empty()) return {};
            int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                static_cast<int>(wide.size()),
                nullptr, 0, nullptr, nullptr);
            if (len <= 0) return {};
            std::string utf8(static_cast<size_t>(len), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                static_cast<int>(wide.size()),
                utf8.data(), len, nullptr, nullptr);
            return utf8;
        }

    } // namespace port

    // ============================================================================
    // 内部实现（匿名命名空间，内部链接）
    // ============================================================================
    namespace {

        std::string WinErrorMsg(const std::string& prefix) {
            DWORD err = ::GetLastError();
            char buf[256] = {};
            ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
            std::string msg(prefix);
            msg += ": ";
            msg += buf;
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();
            return msg;
        }

        // ============================================================================
        // WinWritableFile
        // ============================================================================
        class WinWritableFile : public WritableFile {
        public:
            WinWritableFile(HANDLE h, uint64_t initial_size)
                : handle_(h), offset_(initial_size) {
            }

            ~WinWritableFile() override {
                if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
            }

            Status Append(const Slice& data) override {
                std::lock_guard lk(mu_);
                return WriteAtLocked(data, offset_);
            }

            Status AppendAt(const Slice& data, uint64_t offset) override {
                std::lock_guard lk(mu_);
                return WriteAtLocked(data, offset);
            }

            Status Flush() override { return Sync(); }

            Status Sync() override {
                std::lock_guard lk(mu_);
                if (!::FlushFileBuffers(handle_))
                    return Status::IOError(WinErrorMsg("FlushFileBuffers"));
                return Status::OK();
            }

            uint64_t Size() const override {
                std::lock_guard lk(mu_);
                return offset_;
            }

        private:
            Status WriteAtLocked(const Slice& data, uint64_t offset) {
                if (data.size() == 0) return Status::OK();
                const uint64_t max_offset = static_cast<uint64_t>(
                    (std::numeric_limits<int64_t>::max)());
                if (data.size() > max_offset || offset > max_offset - data.size())
                    return Status::InvalidArgument("file offset out of range");

                size_t written = 0;
                while (written < data.size()) {
                    LARGE_INTEGER position{};
                    position.QuadPart = static_cast<LONGLONG>(offset + written);
                    if (!::SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN))
                        return Status::IOError(WinErrorMsg("SetFilePointerEx"));

                    DWORD to_write = static_cast<DWORD>(
                        std::min<uint64_t>(data.size() - written, 1ull << 30));
                    DWORD n = 0;
                    if (!::WriteFile(handle_, data.data() + written, to_write,
                        &n, nullptr))
                        return Status::IOError(WinErrorMsg("WriteFile"));
                    if (n == 0) return Status::IOError("WriteFile returned 0");
                    written += n;
                }
                if (offset + written > offset_) offset_ = offset + written;
                return Status::OK();
            }

            HANDLE handle_;
            mutable std::mutex mu_;
            uint64_t offset_ = 0;
        };

        // ============================================================================
        // WinRandomAccessFile
        // ============================================================================
        class WinRandomAccessFile : public RandomAccessFile {
        public:
            WinRandomAccessFile(HANDLE sync_handle, HANDLE async_handle, uint64_t size)
                : sync_handle_(sync_handle), async_handle_(async_handle), size_(size) {
            }

            ~WinRandomAccessFile() override {
                if (sync_handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(sync_handle_);
                if (async_handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(async_handle_);
            }

            Status Read(uint64_t offset, size_t n,
                Slice* result, char* scratch) const override {
                if (offset >= size_) {
                    *result = Slice();
                    return Status::OK();
                }
                size_t to_read = std::min<uint64_t>(n, size_ - offset);
                if (to_read == 0) {
                    *result = Slice();
                    return Status::OK();
                }
                const uint64_t max_offset = static_cast<uint64_t>(
                    (std::numeric_limits<int64_t>::max)());
                if (to_read > max_offset - offset)
                    return Status::InvalidArgument("file range out of range");
                if (scratch == nullptr)
                    return Status::IOError("null scratch buffer");
                if (offset > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
                    return Status::InvalidArgument("file offset out of range");

                std::lock_guard lk(mu_);
                size_t done = 0;
                while (done < to_read) {
                    LARGE_INTEGER position{};
                    position.QuadPart = static_cast<LONGLONG>(offset + done);
                    if (!::SetFilePointerEx(sync_handle_, position, nullptr, FILE_BEGIN))
                        return Status::IOError(WinErrorMsg("SetFilePointerEx"));

                    DWORD chunk = static_cast<DWORD>(
                        std::min<uint64_t>(to_read - done, 1ull << 30));
                    DWORD got = 0;
                    if (!::ReadFile(sync_handle_, scratch + done, chunk, &got, nullptr)) {
                        if (::GetLastError() == ERROR_HANDLE_EOF) break;
                        return Status::IOError(WinErrorMsg("ReadFile"));
                    }
                    if (got == 0) break;
                    done += got;
                }
                *result = Slice(scratch, done);
                return Status::OK();
            }

            uint64_t Size() const override { return size_; }

            void* NativeHandle() const override {
                return reinterpret_cast<void*>(async_handle_);
            }

        private:
            HANDLE sync_handle_;
            HANDLE async_handle_;
            uint64_t size_;
            mutable std::mutex mu_;
        };

        // ============================================================================
        // WinFileSystem
        // ============================================================================
        class WinFileSystem : public FileSystem {
        public:
            Status NewWritableFile(const std::string& path,
                std::unique_ptr<WritableFile>* result) override {
                HANDLE h = CreateHandle(path, GENERIC_WRITE, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL);
                if (h == INVALID_HANDLE_VALUE)
                    return Status::IOError(WinErrorMsg("CreateFile(W): " + path));
                *result = std::make_unique<WinWritableFile>(h, 0);
                return Status::OK();
            }

            Status NewAppendableFile(const std::string& path,
                std::unique_ptr<WritableFile>* result) override {
                HANDLE h = CreateHandle(path, GENERIC_WRITE | GENERIC_READ,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL);
                if (h == INVALID_HANDLE_VALUE)
                    return Status::IOError(WinErrorMsg("CreateFile(A): " + path));

                LARGE_INTEGER sz{};
                if (!::GetFileSizeEx(h, &sz)) {
                    ::CloseHandle(h);
                    return Status::IOError(WinErrorMsg("GetFileSizeEx: " + path));
                }
                *result = std::make_unique<WinWritableFile>(
                    h, static_cast<uint64_t>(sz.QuadPart));
                return Status::OK();
            }

            Status NewRandomAccessFile(const std::string& path,
                std::unique_ptr<RandomAccessFile>* result) override {
                HANDLE sync_handle = CreateHandle(path, GENERIC_READ, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL);
                if (sync_handle == INVALID_HANDLE_VALUE)
                    return Status::IOError(WinErrorMsg("CreateFile(R): " + path));

                HANDLE async_handle = CreateHandle(path, GENERIC_READ, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
                if (async_handle == INVALID_HANDLE_VALUE) {
                    DWORD err = ::GetLastError();
                    ::CloseHandle(sync_handle);
                    ::SetLastError(err);
                    return Status::IOError(WinErrorMsg("CreateFile(R, overlapped): " + path));
                }

                LARGE_INTEGER sz{};
                if (!::GetFileSizeEx(sync_handle, &sz)) {
                    DWORD err = ::GetLastError();
                    ::CloseHandle(sync_handle);
                    ::CloseHandle(async_handle);
                    ::SetLastError(err);
                    return Status::IOError(WinErrorMsg("GetFileSizeEx: " + path));
                }
                *result = std::make_unique<WinRandomAccessFile>(
                    sync_handle, async_handle, static_cast<uint64_t>(sz.QuadPart));
                return Status::OK();
            }

            Status FileExists(const std::string& path) override {
                std::wstring wpath = port::Utf8ToWide(path);
                DWORD attrs = ::GetFileAttributesW(wpath.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) return Status::OK();

                DWORD err = ::GetLastError();
                if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                    return Status::NotFound(path);
                ::SetLastError(err);
                return Status::IOError(WinErrorMsg("GetFileAttributesW: " + path));
            }

            Status GetFileSize(const std::string& path, uint64_t* size) override {
                std::wstring wpath = port::Utf8ToWide(path);
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (!::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &info))
                    return Status::IOError(WinErrorMsg("GetFileAttributesEx: " + path));
                *size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                    static_cast<uint64_t>(info.nFileSizeLow);
                return Status::OK();
            }

            // ★ 因为 #undef DeleteFile，这里的 DeleteFile 是真实方法名
            Status DeleteFile(const std::string& path) override {
                std::wstring wpath = port::Utf8ToWide(path);
                if (!::DeleteFileW(wpath.c_str()))
                    return Status::IOError(WinErrorMsg("DeleteFileW: " + path));
                return Status::OK();
            }

            Status TruncateFile(const std::string& path, uint64_t size) override {
                if (size > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
                    return Status::InvalidArgument("truncate size out of range");
                std::wstring wpath = port::Utf8ToWide(path);
                HANDLE handle = ::CreateFileW(wpath.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                    return Status::IOError(WinErrorMsg("CreateFile(truncate): " + path));
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(size);
                BOOL ok = ::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) &&
                    ::SetEndOfFile(handle);
                DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
                if (ok) ok = ::FlushFileBuffers(handle);
                if (!ok && error == ERROR_SUCCESS) error = ::GetLastError();
                ::CloseHandle(handle);
                if (!ok) {
                    ::SetLastError(error);
                    return Status::IOError(WinErrorMsg("truncate: " + path));
                }
                return Status::OK();
            }

            Status RenameFile(const std::string& src, const std::string& dst) override {
                std::wstring wsrc = port::Utf8ToWide(src);
                std::wstring wdst = port::Utf8ToWide(dst);
                if (!::MoveFileExW(wsrc.c_str(), wdst.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return Status::IOError(WinErrorMsg("MoveFileExW: " + src + " -> " + dst));
                return Status::OK();
            }

            Status CreateDirIfMissing(const std::string& path) override {
                std::wstring wpath = port::Utf8ToWide(path);
                if (!::CreateDirectoryW(wpath.c_str(), nullptr)) {
                    DWORD err = ::GetLastError();
                    if (err != ERROR_ALREADY_EXISTS)
                        return Status::IOError(WinErrorMsg("CreateDirectoryW: " + path));
                }
                return Status::OK();
            }

            Status GetChildren(const std::string& dir,
                std::vector<std::string>* result) override {
                std::wstring pattern = port::Utf8ToWide(dir + "/*");
                WIN32_FIND_DATAW data{};
                HANDLE h = ::FindFirstFileW(pattern.c_str(), &data);
                if (h == INVALID_HANDLE_VALUE) {
                    DWORD err = ::GetLastError();
                    if (err == ERROR_FILE_NOT_FOUND) {
                        result->clear();
                        return Status::OK();
                    }
                    return Status::IOError(WinErrorMsg("FindFirstFileW: " + dir));
                }
                result->clear();
                do {
                    const wchar_t* name = data.cFileName;
                    if (std::wcscmp(name, L".") == 0 || std::wcscmp(name, L"..") == 0)
                        continue;
                    result->push_back(port::WideToUtf8(name));
                } while (::FindNextFileW(h, &data));
                ::FindClose(h);
                return Status::OK();
            }

            Status SyncDir(const std::string& dir) override {
                std::wstring wdir = port::Utf8ToWide(dir);
                HANDLE h = ::CreateFileW(wdir.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
                if (h == INVALID_HANDLE_VALUE) return Status::OK();
                BOOL ok = ::FlushFileBuffers(h);
                ::CloseHandle(h);
                if (!ok) return Status::IOError(WinErrorMsg("FlushFileBuffers(dir): " + dir));
                return Status::OK();
            }

        private:
            static HANDLE CreateHandle(const std::string& path,
                DWORD desired_access,
                DWORD creation,
                DWORD attrs) {
                std::wstring wpath = port::Utf8ToWide(path);
                return ::CreateFileW(wpath.c_str(), desired_access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, creation, attrs, nullptr);
            }
        };

    } // namespace 匿名

    // ============================================================================
    // Default()
    // ============================================================================
    FileSystem* FileSystem::Default() {
        if (auto* override = DefaultForTesting()) return override;
        static WinFileSystem fs;
        return &fs;
    }

} // namespace lsm