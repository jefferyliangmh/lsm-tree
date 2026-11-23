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
    src/metrics.cpp
    src/scheduler.cpp
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
    target_compile_definitions(lsm PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(lsm PRIVATE /W4 /utf-8)
        target_compile_definitions(lsm PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        # MinGW, including cross compilation from Linux. Do not pass MSVC flags.
        target_compile_options(lsm PRIVATE -Wall -Wextra -Wpedantic -O3)
        find_package(Threads REQUIRED)
        target_link_libraries(lsm PUBLIC Threads::Threads)
    endif()

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
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    elseif(WIN32)
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

add_executable(lsm_test_advanced tests/test_advanced.cpp)
target_link_libraries(lsm_test_advanced PRIVATE lsm)
set_platform_options(lsm_test_advanced)
add_test(NAME advanced COMMAND lsm_test_advanced)

add_executable(lsm_bench_amp benchmarks/bench_amplification.cpp)
target_link_libraries(lsm_bench_amp PRIVATE lsm)
set_platform_options(lsm_bench_amp)

add_executable(lsm_bench_memtable benchmarks/bench_memtable.cpp)
target_link_libraries(lsm_bench_memtable PRIVATE lsm)
set_platform_options(lsm_bench_memtable)

# Same harness as the RocksDB binary, compiled against this engine.
add_executable(lsm_bench_external benchmarks/bench_external_compare.cpp)
target_link_libraries(lsm_bench_external PRIVATE lsm)
set_platform_options(lsm_bench_external)

# Optional RocksDB comparison. Present on Debian as librocksdb-dev.
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h)
find_library(ROCKSDB_LIBRARY rocksdb)
if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
    add_executable(lsm_bench_rocksdb benchmarks/bench_external_compare.cpp)
    target_include_directories(lsm_bench_rocksdb PRIVATE ${ROCKSDB_INCLUDE_DIR})
    target_compile_definitions(lsm_bench_rocksdb PRIVATE COMPARE_ROCKSDB)
    target_link_libraries(lsm_bench_rocksdb PRIVATE ${ROCKSDB_LIBRARY})
    set_platform_options(lsm_bench_rocksdb)
    message(STATUS "RocksDB comparison benchmark enabled")
else()
    message(STATUS "RocksDB not found: lsm_bench_rocksdb will not be built")
endif()