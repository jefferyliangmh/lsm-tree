# 并发模型

本库的写路径、flush 和 compaction 按下面的方式重叠，而不是共用一把贯穿 I/O 的大锁。

## 角色

| 角色 | 数量 | 职责 |
| --- | --- | --- |
| 前台写线程 | 调用方任意多个 | WAL group commit 后把记录写入当前 memtable。读者不拿 `memtable_mu_`。 |
| flush 池 | `flush_threads`（默认 1） | 把最老的 immutable memtable 落成 L0 SST。`flush_mu_` 保证代际顺序。 |
| compaction 池 | `compaction_threads`（默认 1） | 挑选并执行一次 compaction。同一 DB 同时只有一个 compaction（`compaction_mu_`）。 |
| 协调线程 | 1 | 周期唤醒、补投递丢失的任务、回收 SuperVersion、按窗口调 Bloom。 |

两个池的队列分开、互不阻塞。`Submit` 只入队，不会在持有 compaction 锁时等待队列。

## 一次写

1. `WaitForWriteCapacity`：immutable 队列达到 `immutable_stop_threshold` 时等待；达到 `immutable_slowdown_threshold`，或 compaction 在途+排队达到 `max_compaction_backlog` 时短睡。
2. 共享 `write_mu_`，WAL 组提交。
3. `memtable_mu_` 下写入 memtable。skiplist 的 next 指针用 release 发布；append-only memtable 用 `atomic<shared_ptr<Snapshot>>` 发布不可变索引，记录本体在稳定 chunk 里不再搬迁。
4. 超过 memtable 阈值后旋转出 immutable，并向 flush 池投递任务。flush 完成后再向 compaction 池投递。flush 写 SST 失败时，下一次投递不早于 `bg_interval_ms`，避免失败后空转把队列立刻排空；协调线程到点会再投递。

## compaction 流水线

`pipelined_compaction_io=true`（默认）时，归并前用 `AsyncIOBackend` 把输入 SST 的数据块批量读进 block cache（Linux io_uring，失败则并行 pread；Windows 用 OVERLAPPED 句柄）。归并线程随后命中缓存。预取失败会降级成同步读，不改变归并结果。

文件号在每产出一个 SST 时现取，不再在整个归并期间占住 `file_number_mu_`，因此 flush 可以和 compaction 的预取/归并重叠。`Stats::io_overlap_events` 统计一侧已经在跑时另一侧开始的次数。

## 读

`Get` 拿到 `SuperVersion` 后不再拿 memtable 写锁。L0 以及 size-tiered / hybrid 年轻层按文件号从新到旧扫描（这些层允许 key 重叠）。纯 leveled 的 L1+ 仍二分。重叠检测不信任“用户没改过策略”以外的布局：tiered 始终线性扫描，hybrid 只对 `< hybrid_leveled_from` 的层线性扫描。

## 背压

- immutable 队列：慢路径等待 / 停止写入，避免无限堆积 memtable。
- compaction backlog：队列加上正在执行的任务达到 `max_compaction_backlog` 时再减速。
- 同一 DB 不并行两份会选中同一 SST 的 compaction。多线程只用来和 flush、前台写重叠。
