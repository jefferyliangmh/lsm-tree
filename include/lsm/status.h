#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

enum class Code : uint8_t {
    kOk = 0, kNotFound, kCorruption, kIOError, kInvalidArgument, kBusy
};

class Status {
public:
    Status() noexcept = default;
    Status(Code c, std::string m = {}) : code_(c), msg_(std::move(m)) {}

    static Status OK() noexcept { return {}; }
    static Status NotFound(std::string_view m = {}) { return {Code::kNotFound, std::string(m)}; }
    static Status Corruption(std::string_view m = {}) { return {Code::kCorruption, std::string(m)}; }
    static Status IOError(std::string_view m = {}) { return {Code::kIOError, std::string(m)}; }
    static Status InvalidArgument(std::string_view m = {}) { return {Code::kInvalidArgument, std::string(m)}; }

    bool ok()           const noexcept { return code_ == Code::kOk; }
    bool IsNotFound()   const noexcept { return code_ == Code::kNotFound; }
    bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }
    Code code()         const noexcept { return code_; }
    const std::string& message() const noexcept { return msg_; }

    std::string ToString() const {
        static const char* n[] = {"OK","NotFound","Corruption","IOError","InvalidArgument","Busy"};
        std::string s(n[static_cast<int>(code_)]);
        if (!msg_.empty()) { s += ": "; s += msg_; }
        return s;
    }

private:
    Code        code_ = Code::kOk;
    std::string msg_;
};

} // namespace lsm
