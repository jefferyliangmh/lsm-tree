#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include "lsm/arena.h"
#include "lsm/slice.h"

namespace lsm {

    // ============================================================================
    // 无锁跳表，采用 RocksDB InlineSkipList 风格的紧凑节点布局
    //
    // 节点内存布局（单次 Arena 分配，连续）：
    //   [Node 头: height, key_size, value_size]        (12 字节，对齐到 16)
    //   [std::atomic<Node*> next[0]]                    (8 字节)
    //   [std::atomic<Node*> next[1]]                    (8 字节)
    //   ...
    //   [std::atomic<Node*> next[height-1]]
    //   [key bytes]                                     (key_size 字节)
    //   [value bytes]                                   (value_size 字节)
    //
    // 优势：
    //   1. 一次分配（头 + next 数组 + key + value），无额外堆分配
    //   2. next[0] 和 key 在相邻内存，缓存行命中率高
    //   3. 变长存储，无 std::string 的 SSO/堆分配开销
    //   4. 与无锁 CAS 写入兼容（release/acquire 内存序保护节点内容可见性）
    //
    // Iterator 接口变化：
    //   - 旧: const std::string& key() / value()
    //   - 新: Slice Key() / Value()
    //   减少一次 const char* → std::string 的隐式转换
    // ============================================================================
    template <typename Comparator>
    class SkipList {
    public:
        static constexpr int kMaxHeight = 12;
        static constexpr int kBranching = 4;

        SkipList(Comparator cmp, Arena* arena)
            : cmp_(cmp), arena_(arena), rng_(0x9e3779b9u) {
            head_ = NewNode(Slice{}, Slice{}, kMaxHeight);
        }

        // ========================================================================
        // 紧凑节点：所有数据从 Arena 单次分配，通过静态方法访问
        // ========================================================================
        struct Node {
            int      height;
            uint32_t key_size;
            uint32_t value_size;

            std::atomic<Node*>* NextArray() noexcept {
                return reinterpret_cast<std::atomic<Node*>*>(
                    reinterpret_cast<char*>(this) + sizeof(Node));
            }
            const std::atomic<Node*>* NextArray() const noexcept {
                return reinterpret_cast<const std::atomic<Node*>*>(
                    reinterpret_cast<const char*>(this) + sizeof(Node));
            }

            std::atomic<Node*>& Next(int level) noexcept {
                return NextArray()[level];
            }
            const std::atomic<Node*>& Next(int level) const noexcept {
                return NextArray()[level];
            }

            const char* KeyData() const noexcept {
                return reinterpret_cast<const char*>(NextArray()) +
                    sizeof(std::atomic<Node*>) * height;
            }
            const char* ValueData() const noexcept {
                return KeyData() + key_size;
            }

            Slice Key()   const noexcept { return Slice(KeyData(), key_size); }
            Slice Value() const noexcept { return Slice(ValueData(), value_size); }
        };

        // ========================================================================
        // 插入：当前由外部串行化（DB::memtable_mu_）
        // 下一阶段会改为 CAS 无锁写入
        // ========================================================================
        void Insert(const std::string& key, const std::string& value) {
            Node* prev[kMaxHeight];
            Node* x = head_;
            Slice target(key);
            for (int i = kMaxHeight - 1; i >= 0; --i) {
                Node* nx = x->Next(i).load(std::memory_order_acquire);
                while (nx && cmp_(nx->Key(), target) < 0) {
                    x = nx;
                    nx = x->Next(i).load(std::memory_order_acquire);
                }
                prev[i] = x;
            }

            int height = RandomHeight();
            int cur = max_height_.load(std::memory_order_relaxed);
            if (height > cur) {
                for (int i = cur; i < height; ++i) prev[i] = head_;
                max_height_.store(height, std::memory_order_relaxed);
            }

            Node* nn = NewNode(Slice(key), Slice(value), height);

            for (int i = 0; i < height; ++i) {
                nn->Next(i).store(prev[i]->Next(i).load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                prev[i]->Next(i).store(nn, std::memory_order_release);
            }
            size_.fetch_add(1, std::memory_order_relaxed);
            bytes_.fetch_add(key.size() + value.size(), std::memory_order_relaxed);
        }

        const Node* FindGE(const Slice& target) const {
            Node* x = head_;
            for (int i = kMaxHeight - 1; i >= 0; --i) {
                Node* nx = x->Next(i).load(std::memory_order_acquire);
                while (nx && cmp_(nx->Key(), target) < 0) {
                    x = nx;
                    nx = x->Next(i).load(std::memory_order_acquire);
                }
            }
            return x->Next(0).load(std::memory_order_acquire);
        }

        size_t Size()  const noexcept { return size_.load(std::memory_order_relaxed); }
        size_t Bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }

        class Iterator {
        public:
            explicit Iterator(const SkipList* l)
                : node_(l->head_->Next(0).load(std::memory_order_acquire)) {
            }
            bool Valid() const noexcept { return node_ != nullptr; }
            Slice Key()   const noexcept { return node_->Key(); }
            Slice Value() const noexcept { return node_->Value(); }
            void Next() { if (node_) node_ = node_->Next(0).load(std::memory_order_acquire); }
        private:
            Node* node_;
        };
        Iterator NewIterator() const { return Iterator(this); }

    private:
        Node* NewNode(const Slice& key, const Slice& value, int height) {
            size_t next_arr_size = sizeof(std::atomic<Node*>) * height;
            size_t total = sizeof(Node) + next_arr_size + key.size() + value.size();
            char* buf = arena_->AllocateAligned(total, alignof(Node));

            Node* node = new (buf) Node{ height,
                                        static_cast<uint32_t>(key.size()),
                                        static_cast<uint32_t>(value.size()) };

            auto* next = reinterpret_cast<std::atomic<Node*>*>(buf + sizeof(Node));
            for (int i = 0; i < height; ++i)
                new (&next[i]) std::atomic<Node*>(nullptr);

            char* key_dst = buf + sizeof(Node) + next_arr_size;
            if (key.size() > 0)
                std::memcpy(key_dst, key.data(), key.size());
            char* value_dst = key_dst + key.size();
            if (value.size() > 0)
                std::memcpy(value_dst, value.data(), value.size());

            return node;
        }

        int RandomHeight() {
            int h = 1;
            while (h < kMaxHeight && (rng_() % kBranching) == 0) ++h;
            return h;
        }

        Node* head_;
        std::atomic<int>    max_height_{ 1 };
        std::atomic<size_t> size_{ 0 };
        std::atomic<size_t> bytes_{ 0 };
        Comparator cmp_;
        Arena* arena_;
        mutable std::mt19937 rng_;
    };

} // namespace lsm