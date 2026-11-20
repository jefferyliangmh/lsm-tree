#include "lsm/version_edit.h"
#include <cstring>

namespace lsm {

namespace {
void PutU64(std::string& s, uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }
void PutU32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void PutStr(std::string& s, const std::string& str) {
    PutU32(s, static_cast<uint32_t>(str.size()));
    s.append(str);
}
bool GetU64(const char*& p, const char* lim, uint64_t& v) {
    if (p + 8 > lim) return false;
    std::memcpy(&v, p, 8); p += 8; return true;
}
bool GetU32(const char*& p, const char* lim, uint32_t& v) {
    if (p + 4 > lim) return false;
    std::memcpy(&v, p, 4); p += 4; return true;
}
bool GetStr(const char*& p, const char* lim, std::string& s) {
    uint32_t n;
    if (!GetU32(p, lim, n)) return false;
    if (p + n > lim) return false;
    s.assign(p, n); p += n; return true;
}
} // namespace

std::string VersionEdit::Encode() const {
    std::string out;
    if (has_last_sequence_) { PutU32(out, 1); PutU64(out, last_sequence_); }
    if (has_log_number_)    { PutU32(out, 2); PutU64(out, log_number_); }
    if (has_next_file_number_) { PutU32(out, 5); PutU64(out, next_file_number_); }

    if (!deleted_files_.empty()) {
        PutU32(out, 3);
        PutU32(out, static_cast<uint32_t>(deleted_files_.size()));
        for (auto& [lvl, num] : deleted_files_) {
            PutU32(out, static_cast<uint32_t>(lvl));
            PutU64(out, num);
        }
    }
    if (!added_files_.empty()) {
        PutU32(out, 4);
        PutU32(out, static_cast<uint32_t>(added_files_.size()));
        for (auto& f : added_files_) {
            PutU32(out, static_cast<uint32_t>(f.level));
            PutU64(out, f.file_number);
            PutU64(out, f.file_size);
            PutStr(out, f.smallest_ikey);
            PutStr(out, f.largest_ikey);
        }
    }
    return out;
}

bool VersionEdit::Decode(const char* data, size_t size) {
    const char* p = data;
    const char* lim = data + size;
    *this = VersionEdit{};

    while (p + 4 <= lim) {
        uint32_t tag;
        std::memcpy(&tag, p, 4); p += 4;
        if (tag == 1) {
            if (!GetU64(p, lim, last_sequence_)) return false;
            has_last_sequence_ = true;
        } else if (tag == 2) {
            if (!GetU64(p, lim, log_number_)) return false;
            has_log_number_ = true;
        } else if (tag == 5) {
            if (!GetU64(p, lim, next_file_number_)) return false;
            has_next_file_number_ = true;
        } else if (tag == 3) {
            uint32_t n;
            if (!GetU32(p, lim, n)) return false;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t lvl; uint64_t num;
                if (!GetU32(p, lim, lvl) || !GetU64(p, lim, num)) return false;
                deleted_files_.push_back({static_cast<int>(lvl), num});
            }
        } else if (tag == 4) {
            uint32_t n;
            if (!GetU32(p, lim, n)) return false;
            for (uint32_t i = 0; i < n; ++i) {
                FileMeta f;
                uint32_t lvl;
                if (!GetU32(p, lim, lvl)) return false;
                f.level = static_cast<int>(lvl);
                if (!GetU64(p, lim, f.file_number)) return false;
                if (!GetU64(p, lim, f.file_size)) return false;
                if (!GetStr(p, lim, f.smallest_ikey)) return false;
                if (!GetStr(p, lim, f.largest_ikey)) return false;
                added_files_.push_back(std::move(f));
            }
        } else {
            return false;
        }
    }
    return p == lim;
}

} // namespace lsm
