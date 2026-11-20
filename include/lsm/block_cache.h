#pragma once
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "lsm/block.h"
#include "lsm/slice.h"

namespace lsm {

    // ============================================================================
    // ShardedLRUCache：分片 LRU Block Cache
    //
    // 设计对标：
    //   - LevelDB ShardedLRUCache：16 路分片，每片独立 LRU + 哈希表[reference:3]
    //   - RocksDB LRUCache：num_shard_bits 可配置，默认根据容量选择[reference:4]
    //   - 64 路分片 + splitmix64 哈希可达到 ~18ns 并发访问[reference:5]
    //
    // 关键设计：
    //   - CacheKey = (file_number, block_offset)，16 字节定长，哈希极快
    //   - 每个分片独立 mutex，多线程读不同分片互不阻塞
    //   - 引用计数：Lookup 返回的 Block 在 Unref 之前不会被驱逐
    //   - 容量满时从 LRU 尾部驱逐，严格上限可选
    // ============================================================================

    class ShardedLRUCache {
    public:
        struct CacheKey {
            uint64_t file_number = 0;
            uint64_t block_offset = 0;

            bool operator==(const CacheKey& o) const {
                return file_number == o.file_number &&
                    block_offset == o.block_offset;
            }
        };

        struct CacheKeyHash {
            size_t operator()(const CacheKey& k) const noexcept {
                uint64_t x = k.file_number * 0x9E3779B97F4A7C15ULL + k.block_offset;
                x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
                x ^= x >> 27; x *= 0x94D049BB133111EBULL;
                x ^= x >> 31;
                return static_cast<size_t>(x);
            }
        };

        explicit ShardedLRUCache(size_t capacity, int num_shard_bits = 4);

        std::shared_ptr<Block> Lookup(const CacheKey& key);
        void Insert(const CacheKey& key, std::string block_data);
        void EvictFile(uint64_t file_number);
        size_t TotalCharge() const;

    private:
        struct Entry {
            std::string data;
            size_t charge = 0;
            std::shared_ptr<Block> block;
            std::list<CacheKey>::iterator lru_pos;
        };

        struct Shard {
            // ★ shared_mutex：Lookup 用 shared_lock，Insert 用 unique_lock
            mutable std::shared_mutex mu;
            std::list<CacheKey> lru;
            std::unordered_map<CacheKey, Entry, CacheKeyHash> table;
            size_t charge = 0;
            size_t capacity = 0;
        };

        Shard& ShardFor(const CacheKey& key);

        std::vector<std::unique_ptr<Shard>> shards_;
        int num_shard_bits_;
    };

} // namespace lsm