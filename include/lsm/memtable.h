#pragma once
#include <cstring>
#include <memory>
#include <string>
#include "lsm/arena.h"
#include "lsm/internal_key.h"
#include "lsm/skiplist.h"

namespace lsm {

    struct InternalComparator {
        int operator()(const Slice& a, const Slice& b) const {
            return CompareInternal(a, b);
        }
    };

    class MemTable {
    public:
        using Table = SkipList<InternalComparator>;

        MemTable() : arena_(std::make_unique<Arena>()),
            table_(std::make_unique<Table>(InternalComparator{}, arena_.get())) {
        }

        void Add(uint64_t seq, const std::string& user_key,
            const std::string& value, ValueType type) {
            std::string ikey = MakeInternalKey(user_key, seq, type);
            table_->Insert(ikey, value);
        }

        struct Lookup {
            bool        found = false;
            std::string value;
            ValueType   type = ValueType::kValue;
        };

        Lookup Get(const std::string& user_key, uint64_t snapshot_seq) const {
            Lookup r;
            std::string target = MakeInternalKey(user_key, snapshot_seq,
                ValueType::kValue);
            const auto* n = table_->FindGE(Slice(target));
            if (!n) return r;
            Slice k = n->Key();
            if (k.size() < 8) return r;
            size_t ul = k.size() - 8;
            if (ul != user_key.size()) return r;
            if (std::memcmp(k.data(), user_key.data(), ul) != 0) return r;
            uint64_t tag = DecodeFixed64BE(k.data() + ul);
            if ((tag >> 8) > snapshot_seq) return r;
            r.found = true;
            Slice v = n->Value();
            r.value.assign(v.data(), v.size());
            r.type = static_cast<ValueType>(tag & 0xFF);
            return r;
        }

        Table::Iterator NewIterator() const { return table_->NewIterator(); }
        size_t Size()  const noexcept { return table_->Size(); }
        size_t Bytes() const noexcept { return table_->Bytes(); }
        size_t MemoryUsage() const noexcept { return arena_->MemoryUsage(); }

    private:
        std::unique_ptr<Arena> arena_;
        std::unique_ptr<Table> table_;
    };

} // namespace lsm