#pragma once
// ============================================================================
// 唯一的平台选择入口。所有平台判断集中在这里。
// 主框架和 src/ 中的文件绝不直接 include 平台头文件。
// ============================================================================
#if defined(_WIN32)
#include "port/port_win.h"
#elif defined(__linux__) || defined(__APPLE__)
#include "port/port_posix.h"
#else
#error "Unsupported platform: only Linux, Windows, macOS are supported"
#endif