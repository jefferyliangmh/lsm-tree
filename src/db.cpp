#include "lsm/db.h"
#include "lsm/env.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace lsm {

    namespace {
        bool ParseWalGeneration(const std::string& name, uint64_t* generation) {
            constexpr const char* prefix = "WAL-";
            constexpr const char* suffix = ".log";
            constexpr size_t prefix_size = 4;
            constexpr size_t suffix_size = 4;
            if (name.size() <= prefix_size + suffix_size ||
                name.compare(0, prefix_size, prefix) != 0 ||
                name.compare(name.size() - suffix_size, suffix_size, suffix) != 0)
                return false;
            const char* begin = name.data() + prefix_size;
            const char* end = name.data() + name.size() - suffix_size;
            const auto parsed = std::from_chars(begin, end, *generation);
            return parsed.ec == std::errc{} && parsed.ptr == end;
        }

        uint64_t ElapsedNanos(std::chrono::steady_clock::time_point started) {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        }
    }

    // ============================================================================
    // 构造与析构
    // ============================================================================

    DB::DB(Options opt)
        : opt_(std::move(opt)),
        block_cache_(std::make_shared<ShardedLRUCache>(
            opt_.block_cache_capacity, opt_.block_cache_shard_bits)),
        versions_(opt_.db_path, opt_, block_cache_) {

        Status st_dir = FileSystem::Default()->CreateDirIfMissing(opt_.db_path);
        if (!st_dir.ok())
            throw std::runtime_error("create db dir: " + st_dir.ToString());

        if (opt_.immutable_stop_threshold == 0 ||
            opt_.immutable_slowdown_threshold > opt_.immutable_stop_threshold ||
            opt_.bg_interval_ms <= 0 ||
            opt_.tiered_fanout < 2 || opt_.hybrid_leveled_from < 1 ||
            opt_.flush_threads < 1 || opt_.compaction_threads < 1 ||
            opt_.bloom_bits_min < 1 || opt_.bloom_bits_max < opt_.bloom_bits_min ||
            opt_.bloom_bits_max > 30 || opt_.max_compaction_backlog == 0)
            throw std::invalid_argument("invalid immutable queue, background, compaction, or bloom options");

        filter_telemetry_ = std::make_shared<FilterTelemetry>();
        filter_telemetry_->tuned_bits.store(std::max(1, opt_.bloom_bits_per_key),
            std::memory_order_relaxed);
        filter_policy_ = MakeFilterPolicy(opt_.filter_kind);
        versions_.SetFilterTelemetry(filter_telemetry_.get());
        policy_ = MakeCompactionPolicy(opt_.compaction_style);
        if (opt_.pipelined_compaction_io)
            compaction_io_ = std::make_unique<PipelinedCompactionIO>();
        else
            compaction_io_ = std::make_unique<SyncCompactionIO>();

        auto st = versions_.Recover();
        if (!st.ok()) throw std::runtime_error("VersionSet recover: " + st.ToString());

        seq_.store(versions_.LastSequence(), std::memory_order_relaxed);

        const uint64_t manifest_sequence = versions_.LastSequence();
        if (manifest_sequence == (std::numeric_limits<uint64_t>::max)())
            throw std::runtime_error("sequence number space exhausted");

        std::vector<std::string> children;
        st = FileSystem::Default()->GetChildren(opt_.db_path, &children);
        if (!st.ok()) throw std::runtime_error("list WAL files: " + st.ToString());
        const bool legacy_wal_exists = std::find(children.begin(), children.end(),
            "wal.log") != children.end();
        std::vector<std::pair<uint64_t, std::string>> wal_generations;
        for (const auto& child : children) {
            uint64_t generation = 0;
            if (ParseWalGeneration(child, &generation))
                wal_generations.push_back({ generation, child });
        }
        std::sort(wal_generations.begin(), wal_generations.end());

        const uint64_t minimum_log = versions_.LogNumber();
        if (minimum_log != 0 && std::none_of(wal_generations.begin(),
            wal_generations.end(), [minimum_log](const auto& entry) {
                return entry.first == minimum_log;
            }))
            throw std::runtime_error("MANIFEST references a missing WAL generation");
        if (minimum_log == 0 && !legacy_wal_exists && wal_generations.empty() &&
            manifest_sequence != 0)
            throw std::runtime_error("database is missing its referenced WAL");

        struct WalInput { uint64_t generation; std::string path; };
        std::vector<WalInput> wal_inputs;
        if (legacy_wal_exists && minimum_log == 0)
            wal_inputs.push_back({ 0, opt_.db_path + "/wal.log" });
        for (const auto& [generation, name] : wal_generations) {
            if (minimum_log == 0 || generation >= minimum_log)
                wal_inputs.push_back({ generation, opt_.db_path + "/" + name });
        }
        if (wal_inputs.empty() && minimum_log == 0 && manifest_sequence == 0)
            wal_inputs.push_back({ 0, opt_.db_path + "/wal.log" });

        mutable_mem_ = std::make_shared<MemTable>(opt_.memtable_type);
        uint64_t recovered_sequence = manifest_sequence;
        uint64_t replay_minimum_log = wal_inputs.empty() ? minimum_log :
            wal_inputs.front().generation;
        for (const auto& input : wal_inputs) {
            if (recovered_sequence == (std::numeric_limits<uint64_t>::max)())
                throw std::runtime_error("sequence number space exhausted");
            WAL recovery(input.path, opt_.sync_wal, recovered_sequence + 1);
            if (!recovery.OpenStatus().ok())
                throw std::runtime_error("open WAL " + input.path + ": " +
                    recovery.OpenStatus().ToString());
            std::vector<WAL::Record> records;
            st = recovery.Recover(recovered_sequence, &records);
            if (!st.ok()) throw std::runtime_error("WAL recover: " + st.ToString());
            for (const auto& record : records) {
                if (record.sequence <= manifest_sequence) continue;
                if (record.sequence <= recovered_sequence)
                    throw std::runtime_error("WAL generations contain duplicate or unordered sequences");
                mutable_mem_->Add(record.sequence, record.key, record.value, record.type);
                recovered_sequence = record.sequence;
            }
        }

        if (!wal_generations.empty()) {
            current_wal_number_ = wal_generations.back().first;
            wal_ = std::make_unique<WAL>(opt_.db_path + "/" +
                wal_generations.back().second, opt_.sync_wal, recovered_sequence + 1,
                opt_.collect_write_path_stats);
        } else {
            current_wal_number_ = 0;
            wal_ = std::make_unique<WAL>(opt_.db_path + "/wal.log", opt_.sync_wal,
                recovered_sequence + 1, opt_.collect_write_path_stats);
        }
        if (!wal_->OpenStatus().ok())
            throw std::runtime_error("open active WAL: " + wal_->OpenStatus().ToString());
        wal_->SetNextSequence(recovered_sequence + 1);
        mutable_log_number_ = replay_minimum_log;
        seq_.store(recovered_sequence, std::memory_order_relaxed);

        PublishSuperVersionLocked();

        scheduler_ = std::make_unique<BackgroundScheduler>(
            opt_.flush_threads, opt_.compaction_threads);
        bg_ = std::thread([this] { BackgroundLoop(); });
    }

    DB::~DB() {
        shutdown_.store(true, std::memory_order_release);
        background_cv_.notify_all();
        memtable_cv_.notify_all();

        // ★ 析构必须 noexcept：任何异常都吞掉，绝不让 std::terminate 触发
        try {
            if (bg_.joinable()) bg_.join();
            if (scheduler_) scheduler_->Shutdown();

            if (versions_.CommitStateUncertain()) return;
            Status flush_status = Flush();
            if (!flush_status.ok()) {
                std::fprintf(stderr, "[DB::~DB] final flush failed: %s\n",
                    flush_status.ToString().c_str());
                return;
            }
            std::lock_guard lk(compaction_mu_);
            Status compact_status = CompactLocked();
            if (!compact_status.ok())
                std::fprintf(stderr, "[DB::~DB] final compaction failed: %s\n",
                    compact_status.ToString().c_str());
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[DB::~DB] suppressed exception: %s\n", e.what());
        }
        catch (...) {
            std::fprintf(stderr, "[DB::~DB] suppressed unknown exception\n");
        }
    }

    // ============================================================================
    // SuperVersion 发布
    // ============================================================================

    void DB::PublishSuperVersionLocked() {
        auto* sv = new SuperVersion();
        sv->mutable_mem = mutable_mem_;
        sv->immutable_mems.reserve(immutable_mems_.size());
        for (const auto& immutable : immutable_mems_)
            sv->immutable_mems.push_back(immutable.memtable);
        sv->version = versions_.Current();
        version_mgr_.Install(sv);
    }

    // ============================================================================
    // 写路径
    // ============================================================================

    Status DB::Put(const std::string& key, const std::string& value) {
        while (true) {
            const auto capacity_started = opt_.collect_write_path_stats
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            Status capacity_status = WaitForWriteCapacity();
            if (opt_.collect_write_path_stats)
                stats_.capacity_wait_ns.fetch_add(ElapsedNanos(capacity_started),
                    std::memory_order_relaxed);
            if (!capacity_status.ok()) return capacity_status;

            const auto write_lock_started = opt_.collect_write_path_stats
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::shared_lock write_lk(write_mu_);
            if (opt_.collect_write_path_stats)
                stats_.write_lock_wait_ns.fetch_add(ElapsedNanos(write_lock_started),
                    std::memory_order_relaxed);
            bool capacity_available = false;
            {
                std::lock_guard lk(memtable_mu_);
                capacity_available = immutable_mems_.size() < opt_.immutable_stop_threshold;
            }
            if (!capacity_available) {
                write_lk.unlock();
                continue;
            }

            uint64_t s = 0;
            const auto wal_started = opt_.collect_write_path_stats
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            auto st = wal_->Append(key, value, ValueType::kValue, &s);
            if (opt_.collect_write_path_stats)
                stats_.wal_append_ns.fetch_add(ElapsedNanos(wal_started),
                    std::memory_order_relaxed);
            if (!st.ok()) {
                if (s != 0) AdvanceFailedSequence(s);
                return st;
            }

            bool need_flush = false;
            {
                const auto memtable_lock_started = opt_.collect_write_path_stats
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                std::lock_guard lk(memtable_mu_);
                if (opt_.collect_write_path_stats)
                    stats_.memtable_lock_wait_ns.fetch_add(ElapsedNanos(memtable_lock_started),
                        std::memory_order_relaxed);
                const auto memtable_add_started = opt_.collect_write_path_stats
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                mutable_mem_->Add(s, key, value, ValueType::kValue);
                if (opt_.collect_write_path_stats)
                    stats_.memtable_add_ns.fetch_add(ElapsedNanos(memtable_add_started),
                        std::memory_order_relaxed);
                stats_.user_write_bytes.fetch_add(key.size() + value.size(),
                    std::memory_order_relaxed);
                // Internal-key ordering allows WAL-completed writes to reach the memtable out of order.
                if (s > seq_.load(std::memory_order_relaxed))
                    seq_.store(s, std::memory_order_release);
                if (mutable_mem_->Size() >= opt_.memtable_entries ||
                    mutable_mem_->Bytes() >= opt_.memtable_bytes) {
                    need_flush = true;
                }
            }
            stats_.puts.fetch_add(1, std::memory_order_relaxed);

            if (need_flush) {
                write_lk.unlock();
                const auto rotation_started = opt_.collect_write_path_stats
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                Status rotate_status = RotateMemtable();
                if (opt_.collect_write_path_stats)
                    stats_.rotation_ns.fetch_add(ElapsedNanos(rotation_started),
                        std::memory_order_relaxed);
                if (!rotate_status.ok())
                    std::fprintf(stderr, "[lsm] automatic memtable rotation deferred: %s\n",
                        rotate_status.ToString().c_str());
                ScheduleFlush();
            }
            return Status::OK();
        }
    }

    Status DB::Delete(const std::string& key) {
        while (true) {
            const auto capacity_started = opt_.collect_write_path_stats
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            Status capacity_status = WaitForWriteCapacity();
            if (opt_.collect_write_path_stats)
                stats_.capacity_wait_ns.fetch_add(ElapsedNanos(capacity_started),
                    std::memory_order_relaxed);
            if (!capacity_status.ok()) return capacity_status;

            const auto write_lock_started = opt_.collect_write_path_stats
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::shared_lock write_lk(write_mu_);
            if (opt_.collect_write_path_stats)
                stats_.write_lock_wait_ns.fetch_add(ElapsedNanos(write_lock_started),
                    std::memory_order_relaxed);
            bool capacity_available = false;
            {
                std::lock_guard lk(memtable_mu_);
                capacity_available = immutable_mems_.size() < opt_.immutable_stop_threshold;
            }
            if (!capacity_available) {
                write_lk.unlock();
                continue;
            }

            uint64_t s = 0;
            const auto wal_started = opt_.collect_write_path_stats
                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            auto st = wal_->Append(key, "", ValueType::kDeletion, &s);
            if (opt_.collect_write_path_stats)
                stats_.wal_append_ns.fetch_add(ElapsedNanos(wal_started),
                    std::memory_order_relaxed);
            if (!st.ok()) {
                if (s != 0) AdvanceFailedSequence(s);
                return st;
            }

            bool need_flush = false;
            {
                const auto memtable_lock_started = opt_.collect_write_path_stats
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                std::lock_guard lk(memtable_mu_);
                if (opt_.collect_write_path_stats)
                    stats_.memtable_lock_wait_ns.fetch_add(ElapsedNanos(memtable_lock_started),
                        std::memory_order_relaxed);
                const auto memtable_add_started = opt_.collect_write_path_stats
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                mutable_mem_->Add(s, key, "", ValueType::kDeletion);
                if (opt_.collect_write_path_stats)
                    stats_.memtable_add_ns.fetch_add(ElapsedNanos(memtable_add_started),
                        std::memory_order_relaxed);
                stats_.user_write_bytes.fetch_add(key.size(), std::memory_order_relaxed);
                // Internal-key ordering allows WAL-completed writes to reach the memtable out of order.
                if (s > seq_.load(std::memory_order_relaxed))
                    seq_.store(s, std::memory_order_release);
                if (mutable_mem_->Size() >= opt_.memtable_entries ||
                    mutable_mem_->Bytes() >= opt_.memtable_bytes) {
                    need_flush = true;
                }
            }
            stats_.puts.fetch_add(1, std::memory_order_relaxed);

            if (need_flush) {
                write_lk.unlock();
                const auto rotation_started = opt_.collect_write_path_stats
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                Status rotate_status = RotateMemtable();
                if (opt_.collect_write_path_stats)
                    stats_.rotation_ns.fetch_add(ElapsedNanos(rotation_started),
                        std::memory_order_relaxed);
                if (!rotate_status.ok())
                    std::fprintf(stderr, "[lsm] automatic memtable rotation deferred: %s\n",
                        rotate_status.ToString().c_str());
                ScheduleFlush();
            }
            return Status::OK();
        }
    }

    void DB::AdvanceFailedSequence(uint64_t sequence) {
        std::lock_guard lk(memtable_mu_);
        if (sequence > seq_.load(std::memory_order_relaxed))
            seq_.store(sequence, std::memory_order_release);
    }

    // ============================================================================
    // 读路径：完全无锁
    // ============================================================================

    Status DB::Get(const std::string& key, std::string* value) {
        SuperVersion* sv = version_mgr_.GetReadView();
        uint64_t snap = seq_.load(std::memory_order_relaxed);
        bool found = false;
        ValueType type = ValueType::kValue;

        auto try_mem = [&](const std::shared_ptr<MemTable>& m) -> bool {
            if (!m) return false;
            auto r = m->Get(key, snap);
            if (r.found) {
                found = true;
                type = r.type;
                if (type == ValueType::kValue) *value = std::move(r.value);
                return true;
            }
            return false;
            };

        if (!try_mem(sv->mutable_mem)) {
            for (auto it = sv->immutable_mems.rbegin();
                it != sv->immutable_mems.rend(); ++it) {
                if (try_mem(*it)) break;
            }
        }

        Status read_status = Status::OK();
        if (!found && sv->version && !sv->version->levels.empty()) {
            Slice target_user(key);

            // L0：线性遍历（key range 重叠，新→旧）
            for (auto& f : sv->version->levels[0]) {
                Slice smallest_user = UserKeyOf(Slice(f.smallest_ikey));
                Slice largest_user = UserKeyOf(Slice(f.largest_ikey));
                if (smallest_user.compare(target_user) > 0) continue;
                if (largest_user.compare(target_user) < 0) continue;

                auto r = f.reader->Get(Slice(key));
                if (!r.status.ok()) {
                    read_status = r.status;
                    break;
                }
                if (r.found) {
                    found = true;
                    type = r.type;
                    if (type == ValueType::kValue) *value = std::move(r.value);
                    break;
                }
            }

            // L1+：leveled 层二分；tiered / hybrid 年轻层按文件号从新到旧线性扫，
            // 因为这些层允许 key range 重叠。
            if (!found && read_status.ok()) {
                const auto overlap_scan = [&](size_t level) {
                    if (opt_.compaction_style == CompactionStyle::kSizeTiered) return true;
                    if (opt_.compaction_style == CompactionStyle::kHybrid &&
                        static_cast<int>(level) < opt_.hybrid_leveled_from)
                        return true;
                    return false;
                };
                for (size_t lvl_i = 1; lvl_i < sv->version->levels.size(); ++lvl_i) {
                    auto& lvl = sv->version->levels[lvl_i];
                    if (lvl.empty()) continue;

                    if (overlap_scan(lvl_i)) {
                        std::vector<size_t> order(lvl.size());
                        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                            return lvl[a].number > lvl[b].number;
                        });
                        for (size_t idx : order) {
                            auto& f = lvl[idx];
                            if (!f.reader) continue;
                            Slice smallest_user = UserKeyOf(Slice(f.smallest_ikey));
                            Slice largest_user = UserKeyOf(Slice(f.largest_ikey));
                            if (smallest_user.compare(target_user) > 0) continue;
                            if (largest_user.compare(target_user) < 0) continue;
                            auto r = f.reader->Get(Slice(key));
                            if (!r.status.ok()) {
                                read_status = r.status;
                                break;
                            }
                            if (r.found) {
                                found = true;
                                type = r.type;
                                if (type == ValueType::kValue) *value = std::move(r.value);
                                break;
                            }
                        }
                        if (found || !read_status.ok()) break;
                        continue;
                    }

                    size_t lo = 0, hi = lvl.size();
                    while (lo < hi) {
                        size_t mid = lo + (hi - lo) / 2;
                        Slice mid_user = UserKeyOf(Slice(lvl[mid].smallest_ikey));
                        if (mid_user.compare(target_user) <= 0) lo = mid + 1;
                        else hi = mid;
                    }
                    if (lo == 0) continue;
                    size_t idx = lo - 1;
                    auto& f = lvl[idx];

                    Slice smallest_user = UserKeyOf(Slice(f.smallest_ikey));
                    Slice largest_user = UserKeyOf(Slice(f.largest_ikey));
                    if (smallest_user.compare(target_user) > 0) continue;
                    if (largest_user.compare(target_user) < 0) continue;

                    auto r = f.reader->Get(Slice(key));
                    if (!r.status.ok()) {
                        read_status = r.status;
                        break;
                    }
                    if (r.found) {
                        found = true;
                        type = r.type;
                        if (type == ValueType::kValue) *value = std::move(r.value);
                        break;
                    }
                }
            }
        }

        sv->Unref();
        stats_.gets.fetch_add(1, std::memory_order_relaxed);
        if (!read_status.ok()) return read_status;
        if (!found) return Status::NotFound();
        if (type == ValueType::kDeletion) return Status::NotFound();
        return Status::OK();
    }

    Status DB::WaitForWriteCapacity() {
        if (versions_.CommitStateUncertain())
            return Status::IOError("database commit state is uncertain; reopen database");
        std::unique_lock lk(memtable_mu_);
        const auto below_stop = [this] {
            return immutable_mems_.size() < opt_.immutable_stop_threshold ||
                shutdown_.load(std::memory_order_acquire) ||
                versions_.CommitStateUncertain();
        };
        while (!below_stop()) {
            stats_.write_stops.fetch_add(1, std::memory_order_relaxed);
            memtable_cv_.wait_for(lk, std::chrono::milliseconds(50), below_stop);
        }
        if (shutdown_.load(std::memory_order_acquire))
            return Status::IOError("database is shutting down");
        if (versions_.CommitStateUncertain())
            return Status::IOError("database commit state is uncertain; reopen database");
        if (opt_.immutable_slowdown_threshold != 0 &&
            immutable_mems_.size() >= opt_.immutable_slowdown_threshold) {
            stats_.write_slowdowns.fetch_add(1, std::memory_order_relaxed);
            memtable_cv_.wait_for(lk,
                std::chrono::milliseconds(opt_.write_slowdown_delay_ms), [this] {
                    return immutable_mems_.size() < opt_.immutable_slowdown_threshold ||
                        shutdown_.load(std::memory_order_acquire) ||
                        versions_.CommitStateUncertain();
                });
            if (shutdown_.load(std::memory_order_acquire))
                return Status::IOError("database is shutting down");
            if (versions_.CommitStateUncertain())
                return Status::IOError("database commit state is uncertain; reopen database");
        }
        if (scheduler_ && opt_.max_compaction_backlog > 0) {
            const size_t backlog =
                (compaction_inflight_.load(std::memory_order_acquire) ? 1u : 0u) +
                scheduler_->CompactionQueued();
            if (backlog >= opt_.max_compaction_backlog) {
                stats_.write_slowdowns.fetch_add(1, std::memory_order_relaxed);
                memtable_cv_.wait_for(lk,
                    std::chrono::milliseconds(opt_.write_slowdown_delay_ms));
                if (shutdown_.load(std::memory_order_acquire))
                    return Status::IOError("database is shutting down");
            }
        }
        return Status::OK();
    }

    Status DB::RotateMemtable() {
        std::unique_lock write_lk(write_mu_);
        if (versions_.CommitStateUncertain())
            return Status::IOError("database commit state is uncertain; reopen database");

        std::shared_ptr<MemTable> old;
        uint64_t last_sequence = 0;
        uint64_t old_log_number = 0;
        {
            std::lock_guard lk(memtable_mu_);
            if (!mutable_mem_ || mutable_mem_->Size() == 0) return Status::OK();
            old = mutable_mem_;
            last_sequence = seq_.load(std::memory_order_acquire);
            old_log_number = mutable_log_number_;
        }

        if (last_sequence == (std::numeric_limits<uint64_t>::max)())
            return Status::Corruption("sequence number space exhausted");
        uint64_t new_log_number = 0;
        Status st;
        {
            std::lock_guard number_lock(file_number_mu_);
            st = versions_.NewFileNumber(&new_log_number);
        }
        if (!st.ok()) return st;

        const std::string new_wal_path = opt_.db_path + "/WAL-" +
            std::to_string(new_log_number) + ".log";
        auto new_wal = std::make_unique<WAL>(new_wal_path, opt_.sync_wal,
            last_sequence + 1, opt_.collect_write_path_stats);
        if (!new_wal->OpenStatus().ok()) {
            st = new_wal->OpenStatus();
            new_wal.reset();
            Status cleanup = FileSystem::Default()->DeleteFile(new_wal_path);
            if (!cleanup.ok() && !cleanup.IsNotFound())
                std::fprintf(stderr, "[lsm] failed to remove unopened WAL: %s\n",
                    cleanup.ToString().c_str());
            return st;
        }
        st = FileSystem::Default()->SyncDir(opt_.db_path);
        if (!st.ok()) {
            new_wal.reset();
            Status cleanup = FileSystem::Default()->DeleteFile(new_wal_path);
            if (!cleanup.ok() && !cleanup.IsNotFound())
                std::fprintf(stderr, "[lsm] failed to remove uncommitted WAL: %s\n",
                    cleanup.ToString().c_str());
            return st;
        }

        {
            std::lock_guard lk(memtable_mu_);
            if (mutable_mem_ != old)
                return Status(Code::kBusy, "memtable changed during rotation");
            immutable_mems_.push_back({ old, old_log_number, last_sequence });
            size_t queue_size = immutable_mems_.size();
            size_t peak = stats_.immutable_memtables_peak.load(std::memory_order_relaxed);
            while (peak < queue_size && !stats_.immutable_memtables_peak.compare_exchange_weak(
                peak, queue_size, std::memory_order_relaxed)) {}
            mutable_mem_ = std::make_shared<MemTable>(opt_.memtable_type);
            mutable_log_number_ = new_log_number;
            current_wal_number_ = new_log_number;
            if (opt_.collect_write_path_stats && wal_) {
                const auto wal_stats = wal_->GetWriteStats();
                stats_.wal_mutex_wait_ns.fetch_add(wal_stats.mutex_wait_ns,
                    std::memory_order_relaxed);
                stats_.wal_queue_wait_ns.fetch_add(wal_stats.queue_wait_ns,
                    std::memory_order_relaxed);
                stats_.wal_append_io_ns.fetch_add(wal_stats.append_io_ns,
                    std::memory_order_relaxed);
                stats_.wal_sync_ns.fetch_add(wal_stats.sync_ns,
                    std::memory_order_relaxed);
                stats_.wal_batches.fetch_add(wal_stats.batches,
                    std::memory_order_relaxed);
                stats_.wal_records.fetch_add(wal_stats.records,
                    std::memory_order_relaxed);
            }
            wal_ = std::move(new_wal);
            PublishSuperVersionLocked();
        }
        memtable_cv_.notify_all();
        return Status::OK();
    }

    Status DB::CleanupObsoleteWals(uint64_t minimum_log_number) {
        if (minimum_log_number == 0) return Status::OK();
        std::vector<std::string> children;
        Status st = FileSystem::Default()->GetChildren(opt_.db_path, &children);
        if (!st.ok()) return st;
        for (const auto& child : children) {
            if (child == "wal.log") {
                st = FileSystem::Default()->DeleteFile(opt_.db_path + "/" + child);
            } else {
                uint64_t generation = 0;
                if (!ParseWalGeneration(child, &generation) ||
                    generation >= minimum_log_number)
                    continue;
                st = FileSystem::Default()->DeleteFile(opt_.db_path + "/" + child);
            }
            if (!st.ok() && !st.IsNotFound()) return st;
        }
        return Status::OK();
    }

    Status DB::FlushOldestImmutable() {
        if (versions_.CommitStateUncertain())
            return Status::IOError("database commit state is uncertain; reopen database");
        ImmutableMemTable pending;
        uint64_t next_log_number = 0;
        {
            std::lock_guard lk(memtable_mu_);
            if (immutable_mems_.empty()) return Status::OK();
            pending = immutable_mems_.front();
            next_log_number = immutable_mems_.size() > 1
                ? immutable_mems_[1].minimum_log_number : mutable_log_number_;
        }

        uint64_t file_number = 0;
        Status st;
        {
            std::lock_guard number_lock(file_number_mu_);
            st = versions_.NewFileNumber(&file_number);
        }
        if (!st.ok()) return st;
        const std::string path = opt_.db_path + "/" + std::to_string(file_number) + ".sst";
        const Options build_opt = TableBuildOptions(0);
        auto builder = std::make_unique<TableBuilder>(path, build_opt);
        auto cleanup_output = [&] {
            builder.reset();
            Status cleanup = FileSystem::Default()->DeleteFile(path);
            if (!cleanup.ok() && !cleanup.IsNotFound())
                std::fprintf(stderr, "[lsm] failed to remove incomplete SST: %s\n",
                    cleanup.ToString().c_str());
        };
        std::string smallest, largest;
        auto it = pending.memtable->NewIterator();
        bool first = true;
        while (it.Valid()) {
            Slice key = it.Key();
            Slice value = it.Value();
            if (first) { smallest.assign(key.data(), key.size()); first = false; }
            largest.assign(key.data(), key.size());
            builder->Add(key, value);
            it.Next();
        }
        st = builder->Finish();
        if (!st.ok()) {
            cleanup_output();
            return st;
        }
        st = FileSystem::Default()->SyncDir(opt_.db_path);
        if (!st.ok()) {
            cleanup_output();
            return st;
        }

        auto reader = TableReader::Open(path, block_cache_);
        if (!reader) {
            cleanup_output();
            return Status::IOError("cannot open flushed SST: " + path);
        }

        VersionEdit edit;
        edit.AddFile(0, file_number, smallest, largest, builder->ApproxSize());
        edit.SetLastSequence(pending.last_sequence);
        edit.SetLogNumber(next_log_number);
        st = versions_.LogAndApply(&edit, { reader });
        if (!st.ok()) {
            if (!versions_.CommitStateUncertain()) {
                Status cleanup = FileSystem::Default()->DeleteFile(path);
                if (!cleanup.ok() && !cleanup.IsNotFound())
                    std::fprintf(stderr, "[lsm] failed to remove uncommitted SST: %s\n",
                        cleanup.ToString().c_str());
            }
            return st;
        }

        {
            std::lock_guard lk(memtable_mu_);
            if (immutable_mems_.empty() ||
                immutable_mems_.front().memtable != pending.memtable)
                return Status::Corruption("immutable flush queue changed unexpectedly");
            immutable_mems_.erase(immutable_mems_.begin());
            PublishSuperVersionLocked();
        }
        memtable_cv_.notify_all();
        stats_.flushes.fetch_add(1, std::memory_order_relaxed);
        stats_.flush_bytes.fetch_add(builder->ApproxSize(), std::memory_order_relaxed);
        st = CleanupObsoleteWals(next_log_number);
        if (!st.ok()) return st;
        ScheduleCompaction();
        return Status::OK();
    }

    void DB::ScheduleFlush() {
        flush_pending_.store(true, std::memory_order_release);
        background_cv_.notify_one();
        ScheduleFlushWork();
    }

    void DB::ScheduleFlushWork() {
        if (shutdown_.load(std::memory_order_acquire) || !scheduler_) return;
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms < flush_retry_not_before_ms_.load(std::memory_order_acquire)) return;
        bool expected = false;
        if (!flush_inflight_.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel))
            return;
        const bool queued = scheduler_->ScheduleFlush([this] {
            bool ok = false;
            try {
                ok = FlushWorkerBody();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[lsm] flush worker exception: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[lsm] flush worker unknown exception\n");
            }
            flush_inflight_.store(false, std::memory_order_release);
            if (!ok || shutdown_.load(std::memory_order_acquire)) return;
            bool more = false;
            {
                std::lock_guard lk(memtable_mu_);
                more = !immutable_mems_.empty() ||
                    (mutable_mem_ && (mutable_mem_->Size() >= opt_.memtable_entries ||
                        mutable_mem_->Bytes() >= opt_.memtable_bytes));
            }
            if (more) ScheduleFlushWork();
        });
        if (!queued) flush_inflight_.store(false, std::memory_order_release);
    }

    bool DB::FlushWorkerBody() {
        flush_running_.fetch_add(1, std::memory_order_acq_rel);
        if (compaction_running_.load(std::memory_order_acquire) > 0)
            stats_.overlap_events.fetch_add(1, std::memory_order_relaxed);
        struct RunningGuard {
            std::atomic<int>& running;
            ~RunningGuard() { running.fetch_sub(1, std::memory_order_acq_rel); }
        } guard{ flush_running_ };

        bool need_rotate = false;
        {
            std::lock_guard lk(memtable_mu_);
            need_rotate = immutable_mems_.empty() && mutable_mem_ &&
                (mutable_mem_->Size() >= opt_.memtable_entries ||
                    mutable_mem_->Bytes() >= opt_.memtable_bytes);
        }
        if (need_rotate) {
            Status st = RotateMemtable();
            if (!st.ok()) {
                std::fprintf(stderr, "[lsm] background rotation failed: %s\n",
                    st.ToString().c_str());
                return false;
            }
        }

        std::lock_guard flush_lock(flush_mu_);
        bool has_immutable = false;
        {
            std::lock_guard lk(memtable_mu_);
            has_immutable = !immutable_mems_.empty();
        }
        if (!has_immutable) return true;

        const auto flush_started = opt_.collect_write_path_stats
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Status st = FlushOldestImmutable();
        if (opt_.collect_write_path_stats) {
            stats_.background_flush_ns.fetch_add(ElapsedNanos(flush_started),
                std::memory_order_relaxed);
            stats_.background_flushes.fetch_add(1, std::memory_order_relaxed);
        }
        if (!st.ok()) {
            std::fprintf(stderr, "[lsm] background flush failed: %s\n", st.ToString().c_str());
            const auto retry_at = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(opt_.bg_interval_ms);
            flush_retry_not_before_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    retry_at.time_since_epoch()).count(),
                std::memory_order_release);
            return false;
        }
        return true;
    }

    // ============================================================================
    // Compaction
    // ============================================================================

    void DB::ScheduleCompaction() {
        compaction_pending_.store(true, std::memory_order_release);
        ScheduleCompactionWork();
    }

    void DB::ScheduleCompactionWork() {
        if (shutdown_.load(std::memory_order_acquire) || !scheduler_) return;
        bool expected = false;
        if (!compaction_inflight_.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel))
            return;
        const bool queued = scheduler_->ScheduleCompaction([this] {
            try {
                CompactionWorkerBody();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[lsm] compaction worker exception: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[lsm] compaction worker unknown exception\n");
            }
            compaction_inflight_.store(false, std::memory_order_release);
            if (!shutdown_.load(std::memory_order_acquire) &&
                compaction_pending_.load(std::memory_order_acquire))
                ScheduleCompactionWork();
        });
        if (!queued) compaction_inflight_.store(false, std::memory_order_release);
    }

    void DB::CompactionWorkerBody() {
        compaction_pending_.store(false, std::memory_order_release);
        std::lock_guard lk(compaction_mu_);
        if (shutdown_.load(std::memory_order_acquire)) return;
        compaction_running_.fetch_add(1, std::memory_order_acq_rel);
        if (flush_running_.load(std::memory_order_acquire) > 0)
            stats_.overlap_events.fetch_add(1, std::memory_order_relaxed);
        Status st = CompactLocked();
        compaction_running_.fetch_sub(1, std::memory_order_acq_rel);
        if (!st.ok())
            std::fprintf(stderr, "[lsm] background compaction stopped: %s\n",
                st.ToString().c_str());
    }

    Status DB::Compact() {
        if (versions_.CommitStateUncertain())
            return Status::IOError("database commit state is uncertain; reopen database");
        std::lock_guard lk(compaction_mu_);
        Status st = CompactLocked();
        compaction_pending_.store(false, std::memory_order_release);
        return st;
    }

    Status DB::CompactLocked() {
        while (true) {
            const uint64_t puts = stats_.puts.load(std::memory_order_relaxed);
            const uint64_t gets = stats_.gets.load(std::memory_order_relaxed);
            opt_.observed_write_read_ratio = static_cast<double>(puts) /
                static_cast<double>(gets == 0 ? 1 : gets);

            auto current = versions_.Current();
            auto inputs = policy_->PickCompaction(*current, opt_);
            if (inputs.empty()) break;

            for (auto& in : inputs) {
                auto st = DoCompaction(in);
                if (!st.ok()) return st;
                stats_.compactions.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::lock_guard lk(memtable_mu_);
                PublishSuperVersionLocked();
            }
        }
        return Status::OK();
    }

    Status DB::DoCompaction(const CompactionPolicy::Input& input) {
        auto current = versions_.Current();

        // 收集 reader（从 Version 里拿，不再重新打开）
        std::vector<std::shared_ptr<TableReader>> readers;
        for (uint64_t num : input.input_file_numbers) {
            bool found = false;
            for (auto& lvl : current->levels) {
                for (auto& f : lvl) {
                    if (f.number == num) {
                        readers.push_back(f.reader);
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) {
                std::fprintf(stderr,
                    "[DoCompaction] reader not found for file %llu\n",
                    (unsigned long long)num);
                return Status::IOError("input SST not in version");
            }
        }

        uint64_t input_bytes = 0;
        for (uint64_t num : input.input_file_numbers) {
            for (auto& lvl : current->levels) {
                for (auto& f : lvl) {
                    if (f.number == num) input_bytes += f.file_size;
                }
            }
        }

        const Options build_opt = TableBuildOptions(input.to_level);
        auto allocate = [this](uint64_t* number) -> Status {
            std::lock_guard number_lock(file_number_mu_);
            return versions_.NewFileNumber(number);
        };
        std::vector<VersionEdit::FileMeta> outputs;
        Status st = compaction_io_->Run(input, readers, opt_.db_path, allocate,
            build_opt, &outputs);
        if (!st.ok()) {
            std::fprintf(stderr, "[DoCompaction] Run failed: %s\n",
                st.ToString().c_str());
            return st;
        }
        stats_.compaction_input_bytes.fetch_add(input_bytes, std::memory_order_relaxed);
        uint64_t output_bytes = 0;
        for (const auto& output : outputs) output_bytes += output.file_size;
        stats_.compaction_output_bytes.fetch_add(output_bytes, std::memory_order_relaxed);

        st = FileSystem::Default()->SyncDir(opt_.db_path);
        if (!st.ok()) {
            for (const auto& output : outputs)
                (void)FileSystem::Default()->DeleteFile(opt_.db_path + "/" +
                    std::to_string(output.file_number) + ".sst");
            return st;
        }

        // 打开输出 reader
        std::vector<std::shared_ptr<TableReader>> out_readers;
        for (auto& m : outputs) {
            std::string path = opt_.db_path + "/" +
                std::to_string(m.file_number) + ".sst";
            auto r = TableReader::Open(path, block_cache_);
            if (!r) {
                std::fprintf(stderr,
                    "[DoCompaction] cannot reopen output SST %s\n", path.c_str());
                for (auto& m2 : outputs)
                    FileSystem::Default()->DeleteFile(
                        opt_.db_path + "/" +
                        std::to_string(m2.file_number) + ".sst");
                return Status::IOError("cannot reopen output SST");
            }
            out_readers.push_back(std::move(r));
        }

        // 构造 VersionEdit
        VersionEdit edit;
        for (uint64_t num : input.input_file_numbers) {
            for (int lvl = 0; lvl < static_cast<int>(current->levels.size()); ++lvl) {
                bool found = false;
                for (auto& f : current->levels[lvl]) {
                    if (f.number == num) {
                        edit.DeleteFile(lvl, num);
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }
        for (auto& m : outputs) {
            edit.AddFile(input.to_level, m.file_number,
                m.smallest_ikey, m.largest_ikey, m.file_size);
        }
        edit.SetLastSequence(seq_.load(std::memory_order_relaxed));

        // ★ LogAndApply，失败则回滚所有输出 SST
        st = versions_.LogAndApply(&edit, out_readers);
        if (!st.ok()) {
            std::fprintf(stderr,
                "[DoCompaction] LogAndApply FAILED (L%d→L%d); preserving inputs and outputs: %s\n",
                input.from_level, input.to_level, st.ToString().c_str());
            return st;
        }

        //std::fprintf(stderr,
        //    "[DoCompaction] OK L%d→L%d: inputs=%zu, outputs=%zu\n",
        //    input.from_level, input.to_level,
        //    input.input_file_numbers.size(), outputs.size());

        // 清理旧 SST
        for (uint64_t num : input.input_file_numbers) {
            if (block_cache_) block_cache_->EvictFile(num);
            Status delete_status = FileSystem::Default()->DeleteFile(
                opt_.db_path + "/" + std::to_string(num) + ".sst");
            if (!delete_status.ok() && !delete_status.IsNotFound())
                std::fprintf(stderr, "[DoCompaction] obsolete SST cleanup failed: %s\n",
                    delete_status.ToString().c_str());
        }

        return Status::OK();
    }

    Status DB::Flush() {
        Status st = RotateMemtable();
        if (!st.ok()) return st;
        std::lock_guard flush_lock(flush_mu_);
        while (true) {
            {
                std::lock_guard memtable_lock(memtable_mu_);
                if (immutable_mems_.empty()) return Status::OK();
            }
            st = FlushOldestImmutable();
            if (!st.ok()) return st;
        }
    }

    // ============================================================================
    // 后台线程
    // ============================================================================

    void DB::BackgroundLoop() {
        while (!shutdown_.load(std::memory_order_acquire)) {
            {
                std::unique_lock lk(background_mu_);
                background_cv_.wait_for(lk,
                    std::chrono::milliseconds(opt_.bg_interval_ms), [this] {
                        return shutdown_.load(std::memory_order_acquire) ||
                            flush_pending_.load(std::memory_order_acquire);
                    });
                if (shutdown_.load(std::memory_order_acquire)) break;
                flush_pending_.store(false, std::memory_order_release);
            }

            bool has_immutable = false;
            bool mutable_over_threshold = false;
            {
                std::lock_guard lk(memtable_mu_);
                has_immutable = !immutable_mems_.empty();
                mutable_over_threshold = mutable_mem_ &&
                    (mutable_mem_->Size() >= opt_.memtable_entries ||
                        mutable_mem_->Bytes() >= opt_.memtable_bytes);
            }

            if (has_immutable || mutable_over_threshold) ScheduleFlushWork();
            if (compaction_pending_.load(std::memory_order_acquire))
                ScheduleCompactionWork();
            version_mgr_.PurgeObsolete();
            RetuneFilter();
        }
    }

    Options DB::TableBuildOptions(int level) const {
        Options build = opt_;
        const int tuned = filter_telemetry_
            ? filter_telemetry_->tuned_bits.load(std::memory_order_relaxed)
            : opt_.bloom_bits_per_key;
        if (filter_policy_)
            build.bloom_bits_per_key = filter_policy_->BitsForLevel(level, tuned, opt_);
        return build;
    }

    void DB::RetuneFilter() {
        if (!filter_policy_ || !filter_telemetry_) return;
        const int current = filter_telemetry_->tuned_bits.load(std::memory_order_relaxed);
        const int next = filter_policy_->Tune(current, opt_, *filter_telemetry_);
        filter_telemetry_->tuned_bits.store(next, std::memory_order_relaxed);
    }

    Stats DB::GetStats() const {
        Stats s;
        s.puts = stats_.puts.load(std::memory_order_relaxed);
        s.gets = stats_.gets.load(std::memory_order_relaxed);
        s.flushes = stats_.flushes.load(std::memory_order_relaxed);
        s.compactions = stats_.compactions.load(std::memory_order_relaxed);
        s.write_slowdowns = stats_.write_slowdowns.load(std::memory_order_relaxed);
        s.write_stops = stats_.write_stops.load(std::memory_order_relaxed);
        s.immutable_memtables_peak = stats_.immutable_memtables_peak.load(
            std::memory_order_relaxed);
        s.write_path.capacity_wait_ns = stats_.capacity_wait_ns.load(std::memory_order_relaxed);
        s.write_path.write_lock_wait_ns = stats_.write_lock_wait_ns.load(std::memory_order_relaxed);
        s.write_path.wal_append_ns = stats_.wal_append_ns.load(std::memory_order_relaxed);
        s.write_path.memtable_lock_wait_ns = stats_.memtable_lock_wait_ns.load(std::memory_order_relaxed);
        s.write_path.memtable_add_ns = stats_.memtable_add_ns.load(std::memory_order_relaxed);
        s.write_path.rotation_ns = stats_.rotation_ns.load(std::memory_order_relaxed);
        s.write_path.background_flush_ns = stats_.background_flush_ns.load(std::memory_order_relaxed);
        s.write_path.background_flushes = stats_.background_flushes.load(std::memory_order_relaxed);
        {
            std::shared_lock write_lk(write_mu_);
            const auto wal_stats = wal_ ? wal_->GetWriteStats() : WAL::WriteStats{};
            s.write_path.wal_mutex_wait_ns = stats_.wal_mutex_wait_ns.load(std::memory_order_relaxed) +
                wal_stats.mutex_wait_ns;
            s.write_path.wal_queue_wait_ns = stats_.wal_queue_wait_ns.load(std::memory_order_relaxed) +
                wal_stats.queue_wait_ns;
            s.write_path.wal_append_io_ns = stats_.wal_append_io_ns.load(std::memory_order_relaxed) +
                wal_stats.append_io_ns;
            s.write_path.wal_sync_ns = stats_.wal_sync_ns.load(std::memory_order_relaxed) +
                wal_stats.sync_ns;
            s.write_path.wal_batches = stats_.wal_batches.load(std::memory_order_relaxed) +
                wal_stats.batches;
            s.write_path.wal_records = stats_.wal_records.load(std::memory_order_relaxed) +
                wal_stats.records;
        }
        {
            std::lock_guard lk(memtable_mu_);
            s.memtable_entries = mutable_mem_ ? mutable_mem_->Size() : 0;
            s.memtable_arena_bytes = mutable_mem_ ? mutable_mem_->MemoryUsage() : 0;
            s.immutable_memtables = immutable_mems_.size();
        }
        auto cur = versions_.Current();
        for (auto& lvl : cur->levels) {
            s.level_files.push_back(lvl.size());
            uint64_t bytes = 0;
            for (auto& f : lvl) bytes += f.file_size;
            s.level_bytes.push_back(bytes);
        }
        s.user_write_bytes = stats_.user_write_bytes.load(std::memory_order_relaxed);
        s.flush_bytes = stats_.flush_bytes.load(std::memory_order_relaxed);
        s.compaction_input_bytes = stats_.compaction_input_bytes.load(std::memory_order_relaxed);
        s.compaction_output_bytes = stats_.compaction_output_bytes.load(std::memory_order_relaxed);
        s.block_reads = EngineCounters::block_reads.load(std::memory_order_relaxed);
        s.block_read_bytes = EngineCounters::block_read_bytes.load(std::memory_order_relaxed);
        s.block_cache_hits = EngineCounters::block_cache_hits.load(std::memory_order_relaxed);
        s.prefetch_blocks = EngineCounters::prefetch_blocks.load(std::memory_order_relaxed);
        s.prefetch_bytes = EngineCounters::prefetch_bytes.load(std::memory_order_relaxed);
        s.io_overlap_events = stats_.overlap_events.load(std::memory_order_relaxed);
        s.tuned_bloom_bits = TunedBloomBits();
        s.memtable_type = lsm::MemTableTypeName(opt_.memtable_type);
        s.filter_kind = FilterKindName();
        s.compaction_io = CompactionIOName();
        if (filter_telemetry_) {
            s.filter_lookups = filter_telemetry_->lookups.load(std::memory_order_relaxed);
            s.filter_rejects = filter_telemetry_->bloom_rejects.load(std::memory_order_relaxed);
            s.filter_false_positives = filter_telemetry_->false_positives.load(std::memory_order_relaxed);
            s.filter_hits = filter_telemetry_->bloom_hits.load(std::memory_order_relaxed);
        }
        if (scheduler_) {
            s.scheduler_flush_tasks = scheduler_->FlushCompleted();
            s.scheduler_compaction_tasks = scheduler_->CompactionCompleted();
            s.flush_queue_depth = scheduler_->FlushQueued();
            s.compaction_queue_depth = scheduler_->CompactionQueued() +
                (compaction_inflight_.load(std::memory_order_relaxed) ? 1 : 0);
        }
        return s;
    }

} // namespace lsm