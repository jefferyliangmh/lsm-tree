#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "lsm/db.h"
#include "lsm/metrics.h"

static double Secs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static uint64_t Percentile(std::vector<uint64_t> samples, double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    size_t idx = static_cast<size_t>(p * (samples.size() - 1));
    return samples[idx];
}

static uint64_t DirectoryBytes(const std::filesystem::path& path) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(path, ec);
        !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file()) total += it->file_size(ec);
    }
    return total;
}

struct Row {
    const char* style;
    lsm::CompactionStyle compaction;
    lsm::MemTableType memtable;
    lsm::FilterKind filter;
};

static void RunRow(const Row& row, int keys, int reads, const std::filesystem::path& root) {
    const auto path = root / row.style;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    lsm::EngineCounters::Reset();

    lsm::Options opt;
    opt.db_path = path.string();
    opt.compaction_style = row.compaction;
    opt.memtable_type = row.memtable;
    opt.filter_kind = row.filter;
    opt.memtable_entries = 2000;
    opt.memtable_bytes = 1ull << 20;
    opt.max_file_size = 256 * 1024;
    opt.level0_trigger = 4;
    opt.tiered_fanout = 4;
    opt.max_bytes_for_level_base = 1ull << 20;
    opt.block_cache_capacity = 8ull << 20;
    opt.pipelined_compaction_io = true;
    opt.sync_wal = false;
    opt.hybrid_write_heavy_ratio = 2.0;

    std::vector<uint64_t> write_us;
    std::vector<uint64_t> read_us;
    write_us.reserve(static_cast<size_t>(keys / 32 + 1));
    read_us.reserve(static_cast<size_t>(reads / 32 + 1));

    double write_sec = 0;
    double read_sec = 0;
    lsm::Stats st;
    {
        lsm::DB db(opt);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < keys; ++i) {
            auto op = std::chrono::steady_clock::now();
            std::string key = "k" + std::to_string(i);
            if (!db.Put(key, "value-" + std::to_string(i)).ok()) {
                std::fprintf(stderr, "put failed at %d\n", i);
                return;
            }
            if ((i & 31) == 0) {
                write_us.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - op).count()));
            }
        }
        if (!db.Flush().ok()) {
            std::fprintf(stderr, "flush failed\n");
            return;
        }
        write_sec = Secs(t0);

        t0 = std::chrono::steady_clock::now();
        std::string value;
        int missing = 0;
        for (int i = 0; i < reads; ++i) {
            auto op = std::chrono::steady_clock::now();
            const bool want_miss = (i % 10) == 0;
            std::string key = want_miss ? ("miss-" + std::to_string(i))
                                        : ("k" + std::to_string(i % keys));
            auto status = db.Get(key, &value);
            if (want_miss) {
                if (!status.IsNotFound()) ++missing;
            } else if (!status.ok()) {
                ++missing;
            }
            if ((i & 31) == 0) {
                read_us.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - op).count()));
            }
        }
        read_sec = Secs(t0);
        if (!db.Compact().ok()) {
            std::fprintf(stderr, "compact failed\n");
            return;
        }
        st = db.GetStats();
        if (missing) std::fprintf(stderr, "unexpected read mismatches: %d\n", missing);
    }

    const uint64_t space = DirectoryBytes(path);
    const double user = static_cast<double>(st.user_write_bytes == 0 ? 1 : st.user_write_bytes);
    const double write_amp = static_cast<double>(st.flush_bytes + st.compaction_output_bytes) / user;
    const double space_amp = static_cast<double>(space) / user;
    const double read_amp = static_cast<double>(st.block_reads) /
        static_cast<double>(reads == 0 ? 1 : reads);
    const double fpr_den = static_cast<double>(st.filter_false_positives + st.filter_rejects);
    const double fpr = fpr_den == 0 ? 0 : static_cast<double>(st.filter_false_positives) / fpr_den;

    std::printf("%-14s %-18s %-16s write=%7.0f op/s p99=%6llu us  "
        "read=%7.0f op/s p99=%6llu us  WA=%.2f RA=%.3f SA=%.2f FPR=%.4f "
        "flush=%llu compact_out=%llu prefetch=%llu\n",
        row.style, st.memtable_type.c_str(), st.filter_kind.c_str(),
        keys / (write_sec == 0 ? 1 : write_sec),
        static_cast<unsigned long long>(Percentile(write_us, 0.99)),
        reads / (read_sec == 0 ? 1 : read_sec),
        static_cast<unsigned long long>(Percentile(read_us, 0.99)),
        write_amp, read_amp, space_amp, fpr,
        static_cast<unsigned long long>(st.flush_bytes),
        static_cast<unsigned long long>(st.compaction_output_bytes),
        static_cast<unsigned long long>(st.prefetch_blocks));
    std::filesystem::remove_all(path, ec);
}

int main(int argc, char** argv) {
    int keys = 4000;
    int reads = 4000;
    if (argc > 1) keys = std::atoi(argv[1]);
    if (argc > 2) reads = std::atoi(argv[2]);
    if (keys < 100) keys = 100;
    if (reads < 100) reads = 100;

    const auto root = std::filesystem::temp_directory_path() / "lsm_amp_bench";
    std::filesystem::create_directories(root);
    std::printf("amplification bench keys=%d reads=%d\n", keys, reads);
    std::printf("WA=(flush+compaction bytes)/user bytes; RA=physical block reads/get; "
        "SA=directory bytes/user bytes\n");

    const Row rows[] = {
        { "leveled", lsm::CompactionStyle::kLeveled, lsm::MemTableType::kSkipList,
            lsm::FilterKind::kStandardBloom },
        { "size-tiered", lsm::CompactionStyle::kSizeTiered, lsm::MemTableType::kSkipList,
            lsm::FilterKind::kStandardBloom },
        { "hybrid", lsm::CompactionStyle::kHybrid, lsm::MemTableType::kSkipList,
            lsm::FilterKind::kAdaptiveBloom },
        { "leveled+simd", lsm::CompactionStyle::kLeveled, lsm::MemTableType::kAppendOnlySimd,
            lsm::FilterKind::kLearnedBloom },
    };
    for (const auto& row : rows) RunRow(row, keys, reads, root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 0;
}
