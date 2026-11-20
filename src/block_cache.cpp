#include "lsm/block_cache.h"

#include <algorithm>

namespace lsm {

    ShardedLRUCache::ShardedLRUCache(size_t capacity, int num_shard_bits)
        : num_shard_bits_(num_shard_bits) {
        size_t num_shards = 1ULL << num_shard_bits_;
        shards_.resize(num_shards);
        size_t per_shard = capacity / num_shards;
        for (auto& s : shards_) {
            s = std::make_unique<Shard>();
            s->capacity = per_shard;
        }
    }

    ShardedLRUCache::Shard& ShardedLRUCache::ShardFor(const CacheKey& key) {
        uint64_t h = CacheKeyHash{}(key);
        size_t idx = num_shard_bits_ == 0 ? 0 : h >> (64 - num_shard_bits_);
        return *shards_[idx];
    }

    std::shared_ptr<Block> ShardedLRUCache::Lookup(const CacheKey& key) {
        Shard& s = ShardFor(key);
        std::unique_lock lk(s.mu);
        auto it = s.table.find(key);
        if (it == s.table.end()) return nullptr;

        if (!it->second.block)
            it->second.block = std::make_shared<Block>(it->second.data);
        s.lru.splice(s.lru.begin(), s.lru, it->second.lru_pos);
        return it->second.block;
    }

    void ShardedLRUCache::Insert(const CacheKey& key, std::string block_data) {
        Shard& s = ShardFor(key);
        size_t charge = block_data.size() + 64;

        std::unique_lock lk(s.mu);

        auto it = s.table.find(key);
        if (it != s.table.end()) {
            s.charge -= it->second.charge;
            s.charge += charge;
            it->second.data = std::move(block_data);
            it->second.charge = charge;
            it->second.block.reset();
            s.lru.splice(s.lru.begin(), s.lru, it->second.lru_pos);
            return;
        }

        while (s.charge + charge > s.capacity && !s.lru.empty()) {
            CacheKey victim = s.lru.back();
            s.lru.pop_back();
            auto vit = s.table.find(victim);
            if (vit != s.table.end()) {
                s.charge -= vit->second.charge;
                s.table.erase(vit);
            }
        }

        if (charge > s.capacity) return;

        s.lru.push_front(key);
        Entry e;
        e.data = std::move(block_data);
        e.charge = charge;
        e.lru_pos = s.lru.begin();
        s.charge += charge;
        s.table.emplace(key, std::move(e));
    }

    void ShardedLRUCache::EvictFile(uint64_t file_number) {
        for (auto& s : shards_) {
            std::unique_lock lk(s->mu);
            auto it = s->lru.begin();
            while (it != s->lru.end()) {
                if (it->file_number == file_number) {
                    auto tit = s->table.find(*it);
                    if (tit != s->table.end()) {
                        s->charge -= tit->second.charge;
                        s->table.erase(tit);
                    }
                    it = s->lru.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
    }

    size_t ShardedLRUCache::TotalCharge() const {
        size_t total = 0;
        for (auto& s : shards_) {
            std::shared_lock lk(s->mu);
            total += s->charge;
        }
        return total;
    }

} // namespace lsm