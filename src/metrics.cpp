#include "lsm/metrics.h"

namespace lsm {

    std::atomic<uint64_t> EngineCounters::block_reads{ 0 };
    std::atomic<uint64_t> EngineCounters::block_read_bytes{ 0 };
    std::atomic<uint64_t> EngineCounters::block_cache_hits{ 0 };
    std::atomic<uint64_t> EngineCounters::prefetch_blocks{ 0 };
    std::atomic<uint64_t> EngineCounters::prefetch_bytes{ 0 };

} // namespace lsm
