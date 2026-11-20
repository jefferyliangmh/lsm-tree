cmake_minimum_required(VERSION 3.20)
project(lsm_tree_v2 VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ============================================================================
# 平台无关源文件
#
# 这些文件不含任何平台 #ifdef，只依赖 lsm/env.h 和 lsm/async_io.h 抽象接口。
# 平台判断全部集中在下方的 if(WIN32)/elseif(UNIX) 分派中。
# ============================================================================
add_library(lsm STATIC
    src/block.cpp
    src/block_cache.cpp
    src/compaction.cpp
    src/db.cpp
    src/sstable.cpp
    src/version.cpp
    src/version_edit.cpp
    src/version_set.cpp
    src/wal.cpp
)
target_include_directories(lsm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ============================================================================
# 平台相关源文件：唯一的平台判断点
# ============================================================================
if(WIN32)
    target_sources(lsm PRIVATE
        src/env_win.cpp
        src/async_io_win.cpp
    )
    # /utf-8：源码是 UTF-8，消除 C4819（代码页 936 无法表示中文）
    target_compile_options(lsm PRIVATE /W4 /utf-8)
    target_compile_definitions(lsm PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX              # 禁用 Windows.h 的 min/max 宏
        WIN32_LEAN_AND_MEAN   # 减小 Windows.h 体积
    )
    # IOCP 不需要额外链接库（kernel32.dll 隐式链接）

elseif(UNIX)
    target_sources(lsm PRIVATE
        src/env_posix.cpp
        src/async_io_uring.cpp
    )
    target_compile_options(lsm PRIVATE -Wall -Wextra -Wpedantic -O3)
    find_package(Threads REQUIRED)
    target_link_libraries(lsm PUBLIC Threads::Threads)

    # liburing 自动检测：找到则启用 io_uring，否则退化到 pread
    find_library(URING_LIB uring)
    if(URING_LIB)
        target_compile_definitions(lsm PUBLIC HAS_LIBURING=1)
        target_link_libraries(lsm PUBLIC ${URING_LIB})
        message(STATUS "liburing found: io_uring backend enabled (${URING_LIB})")
    else()
        message(STATUS "liburing not found: async I/O falls back to pread")
    endif()

else()
    message(FATAL_ERROR "Unsupported platform: only Windows and POSIX (Linux/macOS) are supported")
endif()

# ============================================================================
# 测试和 benchmark 目标
#
# Windows 上同样需要 /utf-8，否则源文件里的中文注释会触发 C4819。
# 用函数封装避免重复。
# ============================================================================
function(set_platform_options target)
    if(WIN32)
        target_compile_options(${target} PRIVATE /utf-8)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
endfunction()

enable_testing()

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------
add_executable(lsm_test tests/test_lsm.cpp)
target_link_libraries(lsm_test PRIVATE lsm)
set_platform_options(lsm_test)
add_test(NAME unit COMMAND lsm_test)

add_executable(lsm_crc32c_test tests/test_crc32c.cpp)
target_link_libraries(lsm_crc32c_test PRIVATE lsm)
set_platform_options(lsm_crc32c_test)
add_test(NAME crc32c COMMAND lsm_crc32c_test)

add_executable(lsm_hash_test tests/test_hash_avx2.cpp)
target_link_libraries(lsm_hash_test PRIVATE lsm)
set_platform_options(lsm_hash_test)
add_test(NAME hash_avx2 COMMAND lsm_hash_test)

# ---------------------------------------------------------------------------
# 基准测试
# ---------------------------------------------------------------------------
add_executable(lsm_bench benchmarks/bench.cpp)
target_link_libraries(lsm_bench PRIVATE lsm)
set_platform_options(lsm_bench)

add_executable(lsm_bench_mt benchmarks/bench_mt.cpp)
target_link_libraries(lsm_bench_mt PRIVATE lsm)
set_platform_options(lsm_bench_mt)

add_executable(lsm_crc32c_bench benchmarks/bench_crc32c.cpp)
target_link_libraries(lsm_crc32c_bench PRIVATE lsm)
set_platform_options(lsm_crc32c_bench)

add_executable(lsm_hash_bench benchmarks/bench_hash_avx2.cpp)
target_link_libraries(lsm_hash_bench PRIVATE lsm)
set_platform_options(lsm_hash_bench)