# 相对《总体评估》的实现说明

评估把仓库放在 2025–2026 的 LSM 工作里看，指出六处差距。这次重构把它们做成可开关、有测试的引擎路径，默认 compaction / memtable / filter 仍是 leveled + skiplist + 固定 Bloom，避免改变已有库的恢复语义。

| 评估项 | 现在的代码 |
| --- | --- |
| Tiered / 混合 compaction | `CompactionStyle::{kLeveled,kSizeTiered,kHybrid}`。tiered 只归并本层再晋升，不把下一层拉进输入。hybrid 在 `puts/gets >= hybrid_write_heavy_ratio` 时对 `< hybrid_leveled_from` 的层走 tiered，否则以及更深层走 leveled。 |
| 异步 I/O 进入 compaction | `PipelinedCompactionIO` 在归并前批量预取数据块。flush 池与 compaction 池分离，文件号锁不再覆盖整个归并。 |
| Memtable | `MemTableType::kAppendOnlySimd`：记录追加到稳定 chunk。索引按 user key 分成 32 个有序数组，点查只做一次无分支二分（随机写入时不能靠“扫每个 segment”）。x86-64 上 user key 比较在 AVX2 可用且长度足够时走 32 字节向量路径。 |
| Bloom | `FilterKind::kAdaptiveBloom` 按观测假阳性率调整后续表的 bits，并对 L0 / 深层倾斜。`kLearnedBloom` 在经典 Bloom 前加精确 2 字节前缀模型（无假阴性），前缀太散时退回纯 Bloom。 |
| 并发模型 | 见 [concurrency-model.md](concurrency-model.md)。线程池、队列和背压都在 `BackgroundScheduler` 和 `DB` 里，而不是只靠注释。 |
| 放大与尾延迟 | `benchmarks/bench_amplification.cpp` 输出写放大、读放大（物理块读/Get）、空间放大和 p99。`benchmarks/bench_memtable.cpp` 对比两种 memtable。`bench_external_compare` 在找到 RocksDB 时编译为 `lsm_bench_rocksdb`。 |

读路径对 tiered 层按文件号从新到旧查找，避免“层内重叠时二分漏掉更新的墓碑”。tiered 晋升到已有数据的层时不会把墓碑当成最底层丢掉。

Linux 用系统 g++/CMake 实测；Windows 用 `cmake/mingw-w64-toolchain.cmake` 交叉编译，并用 wine 跑测试二进制。数值在 [measured-results.md](measured-results.md)。这里不写没有跑出来的数字。

Windows 交叉编译：

```bash
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

MSVC 仍走 `/W4 /utf-8`。MinGW 走 gcc 警告选项，不要把 `/utf-8` 传给 gcc。wine 运行 MinGW 产物时，需要把 `libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll` 放在 exe 旁边。本机没有 liburing 时，异步预取降级为线程池 pread。
