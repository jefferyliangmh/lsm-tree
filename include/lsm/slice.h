#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>

namespace lsm {

class Slice {
public:
    Slice() noexcept = default;
    Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}
    Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}
    Slice(std::string_view s) noexcept : data_(s.data()), size_(s.size()) {}

    const char* data()  const noexcept { return data_; }
    size_t      size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }
    char operator[](size_t i) const noexcept { return data_[i]; }

    std::string      ToString() const { return {data_, size_}; }
    std::string_view ToView()   const noexcept { return {data_, size_}; }

    int compare(const Slice& b) const noexcept {
        size_t n = std::min(size_, b.size_);
        int r = (n == 0) ? 0 : std::memcmp(data_, b.data_, n);
        if (r != 0) return r;
        return (size_ < b.size_) ? -1 : (size_ > b.size_ ? 1 : 0);
    }

private:
    const char* data_ = nullptr;
    size_t      size_ = 0;
};

inline bool operator==(const Slice& a, const Slice& b) noexcept { return a.compare(b) == 0; }
inline bool operator!=(const Slice& a, const Slice& b) noexcept { return a.compare(b) != 0; }
inline bool operator< (const Slice& a, const Slice& b) noexcept { return a.compare(b) <  0; }

} // namespace lsm
