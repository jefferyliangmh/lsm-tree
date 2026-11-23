# 实测记录

下面的数字来自这次重构后的一次实跑，不是估算。工作集很小，用来确认路径能跑通并给出同机对比，不能当成生产级放大系数。

## 环境

- Linux：g++，CMake Release，`/tmp/lsm-build`。没有 liburing，compaction 预取走线程池 pread。CRC32C 为 hardware-sse4.2，Bloom 哈希为 avx2-4way。
- Windows：`x86_64-w64-mingw32-g++` 12-posix 交叉编译到 `/tmp/lsm-win`，再用 wine64 8.0 执行。RocksDB 开发包是 Linux 的，MinGW 侧不编 `lsm_bench_rocksdb`。
- RocksDB 对比使用本机 `librocksdb` 7.8.3。

## 测试

| 套件 | Linux | wine64（MinGW exe） |
| --- | --- | --- |
| `lsm_test`（37） | 通过 | 通过 |
| `lsm_test_advanced` | 通过 | 通过 |
| `lsm_crc32c_test` | 通过 | 通过 |
| `lsm_hash_test` | 通过 | 通过 |

## Memtable（5 万随机点查）

`lsm_bench_memtable 50000`。key 是短十进制字符串，AVX2 的 32 字节比较大多用不上，差的是索引形状。

| 结构 | 插入 | 点查 |
| --- | --- | --- |
| skiplist | 2.27e6 ops/s | 2.31e6 ops/s |
| append-only-simd（32 分片有序数组） | 1.24e6 ops/s | 3.29e6 ops/s |

插入更慢，因为每个写入要在分片数组里腾位置。点查更快，因为只做一次二分。

## 放大（8000 写 / 4000 读）

`lsm_bench_amp 8000 4000`。p99 按每 32 次操作采样，样本少，尾延迟只作同一次运行里的相对值。四种配置的空间放大都是 1.63，写放大都在 3.24–3.25：数据量还没把 tiered 和 leveled 的合并形状拉开。

| 配置 | 写 op/s | 读 op/s | WA | RA | FPR |
| --- | --- | --- | --- | --- | --- |
| leveled + skiplist + 固定 Bloom | 5.54e5 | 6.44e5 | 3.25 | 0.006 | 0.0050 |
| size-tiered + skiplist + 固定 Bloom | 5.97e5 | 1.56e6 | 3.25 | 0.006 | 0.0050 |
| hybrid + skiplist + adaptive Bloom | 5.92e5 | 8.70e5 | 3.24 | 0.004 | 0.0251 |
| leveled + simd + learned Bloom | 4.22e5 | 1.66e6 | 3.24 | 0.007 | 0.0251 |

adaptive / learned 的观测假阳性更高，是因为 L0 少给了位，而且 learned 的前缀模型在这个 key 分布上拒绝得不多。读放大是物理块读次数除以 Get，块缓存命中后会很小。

## 与 RocksDB 7.8.3 同口径小样本

两边都是 `bench_external_compare 2000 500 2 500 1`，WAL sync 关、压缩关。本地 LSM 几乎全程还在 memtable 里，RocksDB 的写路径更重。这组数字只说明小写入下本地引擎没有明显回退，不能外推成“全面快于 RocksDB”。

| 引擎 | 顺序写 | 命中读 | 2 线程写 | 2 线程读 | 重启校验 |
| --- | --- | --- | --- | --- | --- |
| local LSM | 9.44e5 ops/s | 2.89e6 ops/s | 1.77e5 ops/s | 2.72e6 ops/s | 通过 |
| RocksDB 7.8.3 | 3.40e5 ops/s | 6.44e5 ops/s | 3.43e5 ops/s | 1.25e6 ops/s | 通过 |

多线程写 RocksDB 更快。顺序写和命中读在这个量级上本地 LSM 更快。
