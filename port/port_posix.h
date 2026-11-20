#pragma once
// ============================================================================
// POSIX 平台原语常量。Linux 和 macOS 共用。
// 注意：macOS 没有 O_DIRECT，kOpenDirectIO 自动降为 0。
// ============================================================================
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lsm {
	namespace port {

		// 打开标志
		constexpr int kOpenReadOnly = O_RDONLY;
		constexpr int kOpenWriteCreate = O_WRONLY | O_CREAT;
		constexpr int kOpenReadWriteCreate = O_RDWR | O_CREAT;
		constexpr int kOpenAppend = O_APPEND;
		constexpr int kOpenTruncate = O_TRUNC;

		// Linux 有 O_DIRECT；macOS 没有，降为 0（等价于不用直接 I/O）
#ifdef O_DIRECT
		constexpr int kOpenDirectIO = O_DIRECT;
#else
		constexpr int kOpenDirectIO = 0;
#endif

		// 默认文件权限
		constexpr mode_t kDefaultFileMode = 0644;

	} // namespace port
} // namespace lsm