# 并发写与混合负载诊断和优化（Windows Release）

## 结论

在既有 8 线程写基准中，DB 写锁获取等待中位数不到 1%；原序号条件变量等待约占被测写阶段的 35%，WAL 端到端耗时约占 42%。据此移除了 `Put`/`Delete` 在 WAL 成功后等待前序写入完成的序号门闩：WAL 仍按序分配序号；SkipList 修改仍由 `memtable_mu_` 串行保护；内部键依靠序号排序；`seq_` 在插入完成后单调更新到最大已插入序号。

相同 5 轮工作负载下，8 线程写吞吐中位数由 **67,407** 提升至 **76,168 ops/s（+13.0%）**。采样写延迟 p50 从 92 µs 降至 68 µs，p95 从 151 µs 降至 137 µs，p99 从 211 µs 降至 204 µs。`Get` 实现及其锁自由读取路径没有改变。并发读阶段中位数为 3,923,223 → 3,752,776 ops/s（观测值 −4.3%）；这项小幅差异可能受 LSM 文件布局和本机负载影响，不能据此宣称严格零退化，后续应在固定数据布局下复测。

新增了真正同时运行 Get/Put 的 50/50 混合阶段，5 轮均无读写错误，且逐项验证并发写入数据、Flush、重开后校验 **340,000 个键**。混合总吞吐中位数为 **140,676 ops/s**；此阶段是为后续比较建立的新基线，没有旧版同一工作负载的可比结果，因此不声称该值代表优化前后的提升。

## 理论与实现依据

- RocksDB 的 [Pipelined Write 说明](https://github.com/meta/rocksdb/wiki/Pipelined-Write)指出，并发写下可以让下一批写者开始 WAL 阶段，同时让上一批继续更新 memtable；文档给出的约 20–30% 提升来自特定的 8 线程、WAL 开启、RAM 文件系统等条件，并非本机性能保证。
- RocksDB 当前主分支的 [WriteThread 实现](https://github.com/facebook/rocksdb/blob/main/db/write_thread.cc)使用写者队列、leader/follower 状态以及短自旋/让出后阻塞的等待策略；[DB 写入实现](https://github.com/facebook/rocksdb/blob/main/db/db_impl/db_impl_write.cc)负责 WAL 和 memtable 写入阶段。该实现适用于 RocksDB 自身的 memtable 和并发协议，不能直接移植到本库。
- 本库 `SkipList::Insert` 的随机高度状态及前驱链接更新不是并发安全操作。因此本次没有并行调用 `Insert`，也没有改动 SkipList 或给 `Get` 增加锁。
- [RocksDB 基本操作文档](https://github.com/meta/rocksdb/wiki/Basic-Operations)还建议用 WriteBatch 批量更新以减少单操作开销。本库当前 DB API 没有批量写接口；单纯给 WAL 增加人为延迟以求更大批次，在探索测试中反而损害吞吐/尾延迟，未保留该改动。

## 实现与正确性边界

- WAL 仍是序号分配点，并保持持久化记录顺序；写成功前仍须完成 WAL append 和 memtable 插入。
- 多个 WAL 成功的写者可按不同于 WAL 序号的次序取得 `memtable_mu_`。memtable 的内部键排序保证同一 user key 的较高序号版本仍排在较低序号版本之前；`seq_` 只单调前进，不会被较晚应用的低序号写回退。
- `AdvanceFailedSequence` 与 memtable 插入共用 `memtable_mu_`，并同样只前进序号高水位。SkipList 写操作仍被互斥保护；SuperVersion、Get 和 block cache 读取算法未改。
- 当前 API 是单键 Get/Put/Delete，没有跨键快照或批量原子写 API。若以后增加可观察的全局快照/多键原子读写，不能仅用“最大已插入序号”描述可能存在的序号空洞；届时应引入连续完成水位或批次发布协议，并补充对应一致性测试。

## Windows Release 对照

| 指标 | 原序号门闩（5 轮中位数） | 移除门闩（5 轮中位数） | 变化 |
|---|---:|---:|---:|
| 8 线程 Put 吞吐 | 67,407 ops/s | 76,168 ops/s | +13.0% |
| 写延迟 p50（每 64 次采样） | 92 µs | 68 µs | −26.1% |
| 写延迟 p95 | 151 µs | 137 µs | −9.3% |
| 写延迟 p99 | 211 µs | 204 µs | −3.3% |
| 8 线程读吞吐 | 3,923,223 ops/s | 3,752,776 ops/s | −4.3% |

固定负载为 8 个写线程、每线程 20,000 次 Put、100,000 个基础键、非同步 WAL、35,000 条 memtable entry 阈值和 4 MiB memtable；每轮均关闭/重开并校验数据。延迟为每 64 次操作采样，微秒整数值；不是全量请求的精确分位数。WAL 非同步模式不保证每次 Put 在机器断电后仍耐久。

原始写对照数据：

- 优化前吞吐：69,946 / 66,700 / 72,871 / 62,865 / 67,407 ops/s；p50：92 / 89 / 92 / 90 / 92 µs。
- 优化后吞吐：63,763 / 78,246 / 76,168 / 79,383 / 68,122 ops/s；p50：67 / 67 / 69 / 68 / 69 µs。
- 混合负载总吞吐：190,664 / 152,123 / 121,254 / 140,676 / 124,334 ops/s；每轮 80,000 Put + 80,000 Get，错误数为 0。

另以诊断关闭配置独立运行了 5 轮，避免把计时开销混入默认配置基线。其写吞吐中位数为 69,342 ops/s，采样写延迟 p50/p95/p99 为 66/135/258 µs；并发读中位数为 3,721,415 ops/s；50/50 混合吞吐中位数为 133,522 ops/s，采样延迟 p50/p95/p99 为 64/125/246 µs。五轮错误数均为 0，重启后均验证 340,000 个键。该组是优化后的无诊断基线，没有对应的优化前无诊断对照，不能用于计算优化收益或与诊断开启结果直接比较。

WAL/DB 计时由 `Options::collect_write_path_stats` 控制，默认关闭；`lsm_bench_mt` 最后一个参数 `1` 开启计时。计时阶段是各写线程累计耗时，阶段占比按各已计时写阶段总和计算；WAL mutex/队列/I/O 数据是 WAL 阶段的子项，不应再次计入总和。后台 flush 与写线程阶段有并行重叠。

## 复现

```powershell
& '.\out\build\x64-release\lsm_bench_mt.exe' 8 100000 20000 0 'C:\lsm_external_compare\mt-diagnostics' 1
```

数据库临时目录位于 `C:\lsm_external_compare\mt-diagnostics\lsm_bench_mt_db_<run-id>`，每轮退出时清理；日志保留为：

- `C:\lsm_external_compare\mt-diagnostics\profile-baseline-*.log`
- `C:\lsm_external_compare\mt-diagnostics\profile-unordered-isolated-*.log`
- `C:\lsm_external_compare\mt-diagnostics\profile-final-mixed-*.log`

## 验证与后续瓶颈

VS Release 构建成功，CTest 3/3 通过；新增测试验证诊断计数有效，既有同 key 并发、WAL generation 和重启测试通过。五轮 benchmark 的并发读、混合读均无错误，逐键验证及重启恢复均通过。

优化后序号等待被移除，观测瓶颈转向 WAL 队列等待和 immutable/backpressure：原始 8 线程 profile 中 WAL 内部平均批大小约从 1.94 条升至 3.02 条，但并发队列等待仍显著；写锁等待仍低。下一步优先评估 WAL leader/follower 与 flush/backpressure 调度，避免直接解除 SkipList 写保护。性能结果来自非隔离本机，适合作为本机回归基线，不是跨平台结论。
