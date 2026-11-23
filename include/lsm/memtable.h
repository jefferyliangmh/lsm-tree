#pragma once
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include "lsm/arena.h"
#include "lsm/flat_memtable.h"
#include "lsm/internal_key.h"
#include "lsm/options.h"
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

        MemTable() : MemTable(MemTableType::kSkipList) {}

        explicit MemTable(MemTableType type) : type_(type) {
            if (type_ == MemTableType::kSkipList) {
                arena_ = std::make_unique<Arena>();
                table_ = std::make_unique<Table>(InternalComparator{}, arena_.get());
            } else {
                flat_ = std::make_unique<AppendOnlySimdTable>();
            }
        }

        MemTableType type() const noexcept { return type_; }

        void Add(uint64_t seq, const std::string& user_key,
            const std::string& value, ValueType type) {
            if (flat_) flat_->Add(seq, user_key, value, type);
            else {
                std::string ikey = MakeInternalKey(user_key, seq, type);
                table_->Insert(ikey, value);
            }
        }

        struct Lookup {
            bool        found = false;
            std::string value;
            ValueType   type = ValueType::kValue;
        };

        Lookup Get(const std::string& user_key, uint64_t snapshot_seq) const {
            if (flat_) {
                auto r = flat_->Get(user_key, snapshot_seq);
                Lookup out;
                out.found = r.found;
                out.value = std::move(r.value);
                out.type = r.type;
                return out;
            }
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

        class Iterator {
        public:
            explicit Iterator(const MemTable* mem)
                : flat_(mem->flat_ != nullptr) {
                if (flat_) flat_it_ = mem->flat_->NewIterator();
                else skip_it_ = mem->table_->NewIterator();
            }

            bool Valid() const noexcept {
                return flat_ ? flat_it_.Valid() : skip_it_.Valid();
            }
            Slice Key() const noexcept {
                return flat_ ? flat_it_.Key() : skip_it_.Key();
            }
            Slice Value() const noexcept {
                return flat_ ? flat_it_.Value() : skip_it_.Value();
            }
            void Next() {
                if (flat_) flat_it_.Next();
                else skip_it_.Next();
            }

        private:
            bool flat_ = false;
            Table::Iterator skip_it_{};
            AppendOnlySimdTable::Iterator flat_it_{};
        };

        Iterator NewIterator() const { return Iterator(this); }
        size_t Size() const noexcept {
            return flat_ ? flat_->Size() : table_->Size();
        }
        size_t Bytes() const noexcept {
            return flat_ ? flat_->Bytes() : table_->Bytes();
        }
        size_t MemoryUsage() const noexcept {
            return flat_ ? flat_->MemoryUsage() : arena_->MemoryUsage();
        }

    private:
        friend class Iterator;
        MemTableType type_;
        std::unique_ptr<Arena> arena_;
        std::unique_ptr<Table> table_;
        std::unique_ptr<AppendOnlySimdTable> flat_;
    };

} // namespace lsm
