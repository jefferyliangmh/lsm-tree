#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(COMPARE_LEVELDB)
#include <leveldb/cache.h>
#include <leveldb/db.h>
#elif defined(COMPARE_ROCKSDB)
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#else
#include "lsm/db.h"
#endif

using Clock = std::chrono::steady_clock;
static double Seconds(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
}

static void PrintLatency(const char* label, std::vector<uint64_t> samples) {
	std::sort(samples.begin(), samples.end());
	const auto at = [&samples](double p) {
		return samples[static_cast<size_t>(p * (samples.size() - 1))];
	};
	std::printf("  [%s-LAT] n=%zu p50=%llu us p95=%llu us p99=%llu us max=%llu us\n",
		label, samples.size(), static_cast<unsigned long long>(at(0.50)),
		static_cast<unsigned long long>(at(0.95)),
		static_cast<unsigned long long>(at(0.99)),
		static_cast<unsigned long long>(samples.back()));
	std::fflush(stdout);
}

class Store {
public:
	bool Open(const std::string& path) {
#if defined(COMPARE_LEVELDB)
		leveldb::Options options;
		options.create_if_missing = true;
		options.compression = leveldb::kNoCompression;
		options.write_buffer_size = 4ull * 1024 * 1024;
		options.max_file_size = 16ull * 1024 * 1024;
		options.block_restart_interval = 8;
		cache_.reset(leveldb::NewLRUCache(512ull * 1024 * 1024));
		options.block_cache = cache_.get();
		leveldb::Status status = leveldb::DB::Open(options, path, &db_);
		if (!status.ok()) SetError(status.ToString());
		return status.ok();
#elif defined(COMPARE_ROCKSDB)
		rocksdb::Options options;
		options.create_if_missing = true;
		options.compression = rocksdb::kNoCompression;
		options.write_buffer_size = 4ull * 1024 * 1024;
		options.max_write_buffer_number = 2;
		options.level0_file_num_compaction_trigger = 4;
		options.target_file_size_base = 16ull * 1024 * 1024;
		options.max_bytes_for_level_base = 256ull * 1024 * 1024;
		options.max_background_jobs = 2;
		rocksdb::BlockBasedTableOptions table_options;
		table_options.block_size = 4 * 1024;
		table_options.block_restart_interval = 8;
		table_options.block_cache = rocksdb::NewLRUCache(512ull * 1024 * 1024);
		options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
		rocksdb::DB* opened = nullptr;
		rocksdb::Status status = rocksdb::DB::Open(options, path, &opened);
		db_.reset(opened);
		if (!status.ok()) SetError(status.ToString());
		return status.ok();
#else
		lsm::Options options;
		options.db_path = path;
		options.sync_wal = false;
		options.level0_trigger = 4;
		options.memtable_entries = 35000;
		options.memtable_bytes = 4ull * 1024 * 1024;
		options.max_file_size = 16ull * 1024 * 1024;
		options.max_bytes_for_level_base = 256ull * 1024 * 1024;
		options.block_cache_capacity = 512ull * 1024 * 1024;
		options.block_cache_shard_bits = 8;
		options.block_restart_interval = 8;
		try {
			db_ = std::make_unique<lsm::DB>(options);
			return true;
		} catch (const std::exception& e) {
			SetError(e.what());
			return false;
		}
#endif
	}

	bool Put(const std::string& key, const std::string& value) {
#if defined(COMPARE_LEVELDB)
		const leveldb::Status status = db_->Put(leveldb::WriteOptions(), key, value);
		if (!status.ok()) SetError(status.ToString());
		return status.ok();
#elif defined(COMPARE_ROCKSDB)
		const rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
		if (!status.ok()) SetError(status.ToString());
		return status.ok();
#else
		const lsm::Status status = db_->Put(key, value);
		if (!status.ok()) SetError(status.ToString());
		return status.ok();
#endif
	}

	bool Get(const std::string& key, std::string* value, bool expect_missing = false) {
#if defined(COMPARE_LEVELDB)
		const leveldb::Status status = db_->Get(leveldb::ReadOptions(), key, value);
		const bool ok = expect_missing ? status.IsNotFound() : status.ok();
		if (!ok) SetError(status.ToString());
		return ok;
#elif defined(COMPARE_ROCKSDB)
		const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, value);
		const bool ok = expect_missing ? status.IsNotFound() : status.ok();
		if (!ok) SetError(status.ToString());
		return ok;
#else
		const lsm::Status status = db_->Get(key, value);
		const bool ok = expect_missing ? status.IsNotFound() : status.ok();
		if (!ok) SetError(status.ToString());
		return ok;
#endif
	}

	bool Close() {
#if defined(COMPARE_LEVELDB)
		delete db_;
		db_ = nullptr;
		cache_.reset();
#elif defined(COMPARE_ROCKSDB)
		db_.reset();
#else
		db_.reset();
#endif
		return true;
	}

	std::string Error() const {
		std::lock_guard<std::mutex> lock(error_mutex_);
		return error_;
	}

private:
	void SetError(std::string error) {
		std::lock_guard<std::mutex> lock(error_mutex_);
		if (error_.empty()) error_ = std::move(error);
	}

#if defined(COMPARE_LEVELDB)
	leveldb::DB* db_ = nullptr;
	std::unique_ptr<leveldb::Cache> cache_;
#elif defined(COMPARE_ROCKSDB)
	std::unique_ptr<rocksdb::DB> db_;
#else
	std::unique_ptr<lsm::DB> db_;
#endif
	mutable std::mutex error_mutex_;
	std::string error_;
};

static bool RunOne(int run, int keys, int reads, int threads, int ops_per_thread,
	const std::filesystem::path& root) {
	const auto path = root / ("run_" + std::to_string(run));
	std::error_code ec;
	std::filesystem::remove_all(path, ec);
	if (ec) {
		std::fprintf(stderr, "cleanup failed for %s: %s\n", path.string().c_str(), ec.message().c_str());
		return false;
	}

	const std::string base_value(100, 'x');
	std::vector<std::string> mixed_keys;
	mixed_keys.reserve(static_cast<size_t>(reads / 2 + 1));
	std::string value;
	Store store;
	auto t = Clock::now();
	if (!store.Open(path.string())) {
		std::fprintf(stderr, "open failed: %s\n", store.Error().c_str());
		return false;
	}
	const double open_seconds = Seconds(t);
	std::printf("[RUN %d] open=%.4fs\n", run, open_seconds);

	std::vector<uint64_t> latency;
	latency.reserve(static_cast<size_t>(keys / 64 + 1));
	t = Clock::now();
	for (int i = 0; i < keys; ++i) {
		const auto op = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
		if (!store.Put("key_" + std::to_string(i), base_value)) {
			std::fprintf(stderr, "base Put %d failed: %s\n", i, store.Error().c_str());
			return false;
		}
		if (i % 64 == 0)
			latency.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - op).count()));
	}
	const double write_seconds = Seconds(t);
	std::printf("  [WRITE] %d puts in %.3fs => %.0f ops/s\n", keys, write_seconds, keys / write_seconds);
	PrintLatency("WRITE", std::move(latency));

	std::mt19937_64 rng(42);
	latency.clear();
	latency.reserve(static_cast<size_t>(reads / 64 + 1));
	t = Clock::now();
	for (int i = 0; i < reads; ++i) {
		const auto op = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
		const std::string key = "key_" + std::to_string(rng() % keys);
		if (!store.Get(key, &value) || value != base_value) {
			std::fprintf(stderr, "hit Get failed (%s): %s\n", key.c_str(), store.Error().c_str());
			return false;
		}
		if (i % 64 == 0)
			latency.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - op).count()));
	}
	const double hit_seconds = Seconds(t);
	std::printf("  [READ-H] %d gets in %.3fs => %.0f ops/s\n", reads, hit_seconds, reads / hit_seconds);
	PrintLatency("READ-H", std::move(latency));

	latency.clear();
	latency.reserve(static_cast<size_t>(reads / 64 + 1));
	t = Clock::now();
	for (int i = 0; i < reads; ++i) {
		const auto op = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
		if (!store.Get("missing_" + std::to_string(i), &value, true)) {
			std::fprintf(stderr, "miss Get failed: %s\n", store.Error().c_str());
			return false;
		}
		if (i % 64 == 0)
			latency.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - op).count()));
	}
	const double miss_seconds = Seconds(t);
	std::printf("  [READ-M] %d gets in %.3fs => %.0f ops/s\n", reads, miss_seconds, reads / miss_seconds);
	PrintLatency("READ-M", std::move(latency));

	rng.seed(7);
	latency.clear();
	latency.reserve(static_cast<size_t>(reads / 64 + 1));
	int mixed_puts = 0;
	t = Clock::now();
	for (int i = 0; i < reads; ++i) {
		const auto op = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
		if (rng() & 1) {
			std::string key = "mixed_" + std::to_string(i);
			if (!store.Put(key, "mixed-value")) {
				std::fprintf(stderr, "mixed Put failed: %s\n", store.Error().c_str());
				return false;
			}
			mixed_keys.push_back(std::move(key));
			++mixed_puts;
		} else {
			const std::string key = "key_" + std::to_string(rng() % keys);
			if (!store.Get(key, &value) || value != base_value) {
				std::fprintf(stderr, "mixed Get failed (%s): %s\n", key.c_str(), store.Error().c_str());
				return false;
			}
		}
		if (i % 64 == 0)
			latency.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - op).count()));
	}
	const double mixed_seconds = Seconds(t);
	std::printf("  [MIXED] %d ops in %.3fs => %.0f ops/s (puts=%d gets=%d)\n",
		reads, mixed_seconds, reads / mixed_seconds, mixed_puts, reads - mixed_puts);
	PrintLatency("MIXED", std::move(latency));

	std::printf("  [MT-WRITE] starting %d threads x %d ops\n", threads, ops_per_thread);
	std::fflush(stdout);
	std::vector<std::vector<uint64_t>> per_thread_latency(threads);
	std::vector<std::thread> workers;
	workers.reserve(static_cast<size_t>(threads));
	std::atomic<bool> start{ false };
	std::atomic<uint64_t> concurrent_errors{ 0 };
	for (int thread = 0; thread < threads; ++thread) {
		workers.emplace_back([&, thread] {
			auto& samples = per_thread_latency[thread];
			samples.reserve(static_cast<size_t>(ops_per_thread / 64 + 1));
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (int i = 0; i < ops_per_thread; ++i) {
				const auto op = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
				const std::string key = "mt_" + std::to_string(thread) + "_" + std::to_string(i);
				const std::string val = std::to_string(thread) + ":" + std::to_string(i);
				if (!store.Put(key, val)) concurrent_errors.fetch_add(1, std::memory_order_relaxed);
				if (i % 64 == 0)
					samples.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - op).count()));
			}
		});
	}
	t = Clock::now();
	start.store(true, std::memory_order_release);
	for (auto& worker : workers) worker.join();
	const double concurrent_write_seconds = Seconds(t);
	if (concurrent_errors.load(std::memory_order_relaxed) != 0) {
		std::fprintf(stderr, "concurrent write failures=%llu (%s)\n",
			static_cast<unsigned long long>(concurrent_errors.load()), store.Error().c_str());
		return false;
	}
	latency.clear();
	for (auto& samples : per_thread_latency)
		latency.insert(latency.end(), samples.begin(), samples.end());
	const uint64_t concurrent_ops = static_cast<uint64_t>(threads) * static_cast<uint64_t>(ops_per_thread);
	std::printf("  [MT-WRITE] %llu ops in %.3fs => %.0f ops/s\n",
		static_cast<unsigned long long>(concurrent_ops), concurrent_write_seconds,
		concurrent_ops / concurrent_write_seconds);
	PrintLatency("MT-WRITE", std::move(latency));

	std::printf("  [MT-READ] starting %d threads x %d ops\n", threads, ops_per_thread);
	std::fflush(stdout);
	per_thread_latency.assign(static_cast<size_t>(threads), {});
	workers.clear();
	start.store(false, std::memory_order_relaxed);
	concurrent_errors.store(0, std::memory_order_relaxed);
	for (int thread = 0; thread < threads; ++thread) {
		workers.emplace_back([&, thread] {
			auto& samples = per_thread_latency[thread];
			samples.reserve(static_cast<size_t>(ops_per_thread / 64 + 1));
			std::mt19937_64 thread_rng(static_cast<uint64_t>(thread) * 123456789ULL + 42);
			std::string thread_value;
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (int i = 0; i < ops_per_thread; ++i) {
				const auto op = (i % 64 == 0) ? Clock::now() : Clock::time_point{};
				const int index = static_cast<int>(thread_rng() % static_cast<uint64_t>(ops_per_thread));
				const std::string key = "mt_" + std::to_string(thread) + "_" + std::to_string(index);
				const std::string expected = std::to_string(thread) + ":" + std::to_string(index);
				if (!store.Get(key, &thread_value) || thread_value != expected)
					concurrent_errors.fetch_add(1, std::memory_order_relaxed);
				if (i % 64 == 0)
					samples.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - op).count()));
			}
		});
	}
	t = Clock::now();
	start.store(true, std::memory_order_release);
	for (auto& worker : workers) worker.join();
	const double concurrent_read_seconds = Seconds(t);
	if (concurrent_errors.load(std::memory_order_relaxed) != 0) {
		std::fprintf(stderr, "concurrent read failures=%llu (%s)\n",
			static_cast<unsigned long long>(concurrent_errors.load()), store.Error().c_str());
		return false;
	}
	latency.clear();
	for (auto& samples : per_thread_latency)
		latency.insert(latency.end(), samples.begin(), samples.end());
	std::printf("  [MT-READ] %llu gets in %.3fs => %.0f ops/s\n",
		static_cast<unsigned long long>(concurrent_ops), concurrent_read_seconds,
		concurrent_ops / concurrent_read_seconds);
	PrintLatency("MT-READ", std::move(latency));

	std::printf("  [CLOSE] draining and closing database\n");
	std::fflush(stdout);
	t = Clock::now();
	store.Close();
	const double close_seconds = Seconds(t);
	std::printf("  [CLOSE] %.3fs\n", close_seconds);

	Store recovered;
	t = Clock::now();
	if (!recovered.Open(path.string())) {
		std::fprintf(stderr, "reopen failed: %s\n", recovered.Error().c_str());
		return false;
	}
	const double reopen_seconds = Seconds(t);
	t = Clock::now();
	for (int i = 0; i < keys; ++i) {
		if (!recovered.Get("key_" + std::to_string(i), &value) || value != base_value) {
			std::fprintf(stderr, "recovery mismatch at base key %d (%s)\n", i, recovered.Error().c_str());
			return false;
		}
	}
	for (const auto& key : mixed_keys) {
		if (!recovered.Get(key, &value) || value != "mixed-value") {
			std::fprintf(stderr, "recovery mismatch at %s (%s)\n", key.c_str(), recovered.Error().c_str());
			return false;
		}
	}
	for (int thread = 0; thread < threads; ++thread) {
		for (int i = 0; i < ops_per_thread; ++i) {
			const std::string expected = std::to_string(thread) + ":" + std::to_string(i);
			const std::string key = "mt_" + std::to_string(thread) + "_" + std::to_string(i);
			if (!recovered.Get(key, &value) || value != expected) {
				std::fprintf(stderr, "recovery mismatch at %s (%s)\n", key.c_str(), recovered.Error().c_str());
				return false;
			}
		}
	}
	const double verify_seconds = Seconds(t);
	std::printf("  [RECOVERY] reopen=%.4fs verify=%.3fs keys=%d+%d+%llu\n",
		reopen_seconds, verify_seconds, keys, mixed_puts,
		static_cast<unsigned long long>(concurrent_ops));
	recovered.Close();
	std::filesystem::remove_all(path, ec);
	if (ec) std::fprintf(stderr, "warning: cannot remove %s: %s\n", path.string().c_str(), ec.message().c_str());
	return true;
}

static bool ParsePositiveInt(const char* text, int maximum, int* value) {
	if (!text || !value) return false;
	const char* end = text;
	while (*end != '\0') ++end;
	int parsed = 0;
	const auto result = std::from_chars(text, end, parsed);
	if (result.ec != std::errc{} || result.ptr != end || parsed <= 0 || parsed > maximum)
		return false;
	*value = parsed;
	return true;
}

int main(int argc, char** argv) {
	int keys = 500000;
	int reads = 500000;
	int threads = 8;
	int ops_per_thread = 200000;
	int runs = 5;
	if (argc != 6 || !ParsePositiveInt(argv[1], (std::numeric_limits<int>::max)(), &keys) ||
		!ParsePositiveInt(argv[2], (std::numeric_limits<int>::max)(), &reads) ||
		!ParsePositiveInt(argv[3], 256, &threads) ||
		!ParsePositiveInt(argv[4], (std::numeric_limits<int>::max)(), &ops_per_thread) ||
		!ParsePositiveInt(argv[5], 100, &runs) ||
		static_cast<uint64_t>(threads) * static_cast<uint64_t>(ops_per_thread) >
			(std::numeric_limits<size_t>::max)()) {
		std::fprintf(stderr, "usage: bench_external_compare <keys> <reads-per-phase> <threads> <ops-per-thread> <runs>\n");
		return 2;
	}
#if defined(COMPARE_LEVELDB)
	const char* engine = "LevelDB 1.23";
#elif defined(COMPARE_ROCKSDB)
	char engine_buf[64];
	std::snprintf(engine_buf, sizeof(engine_buf), "RocksDB %d.%d.%d",
		ROCKSDB_MAJOR, ROCKSDB_MINOR, ROCKSDB_PATCH);
	const char* engine = engine_buf;
#else
	const char* engine = "local LSM";
#endif
	const char* root_env = std::getenv("LSM_BENCH_ROOT");
	const std::filesystem::path root = (root_env && *root_env
		? std::filesystem::path(root_env)
		: std::filesystem::temp_directory_path() / "lsm_external_compare") /
#if defined(COMPARE_LEVELDB)
		"leveldb";
#elif defined(COMPARE_ROCKSDB)
		"rocksdb";
#else
		"local_lsm";
#endif
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	if (ec) {
		std::fprintf(stderr, "cannot create DB root: %s\n", ec.message().c_str());
		return 2;
	}
	std::printf("=== LSM comparison: %s ===\nkeys=%d reads/phase=%d threads=%d ops/thread=%d runs=%d\nDB root: %s\n"
		"WAL sync=off; compression=off; write buffer=4 MiB; cache=512 MiB; "
		"latency sample interval=64 ops\n",
		engine, keys, reads, threads, ops_per_thread, runs, root.string().c_str());
	for (int run = 1; run <= runs; ++run) {
		if (!RunOne(run, keys, reads, threads, ops_per_thread, root)) return 1;
	}
	return 0;
}
