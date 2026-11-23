#pragma once
// ============================================================================
// Windows 平台原语
//
// 文件路径需 UTF-8 std::string ↔ UTF-16 LPCWSTR 转换。
// 文件句柄类型是 HANDLE。
//
// 这些只被 env_win.cpp 使用，主框架不直接依赖。
// ============================================================================
#include <windows.h>
#include <cstdint>
#include <string>

namespace lsm {
	namespace port {

		// UTF-8 ↔ UTF-16 转换
		std::wstring Utf8ToWide(const std::string& utf8);
		std::string  WideToUtf8(const std::wstring& wide);

		// 文件句柄类型
		using FileHandle = HANDLE;

	} // namespace port
} // namespace lsm