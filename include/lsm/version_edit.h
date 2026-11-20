#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "lsm/internal_key.h"

namespace lsm {

class VersionEdit {
public:
    void SetLastSequence(uint64_t seq) { last_sequence_ = seq; has_last_sequence_ = true; }
    bool HasLastSequence() const { return has_last_sequence_; }
    uint64_t LastSequence() const { return last_sequence_; }

    void SetLogNumber(uint64_t num) { log_number_ = num; has_log_number_ = true; }
    bool HasLogNumber() const { return has_log_number_; }
    uint64_t LogNumber() const { return log_number_; }

    void SetNextFileNumber(uint64_t num) { next_file_number_ = num; has_next_file_number_ = true; }
    bool HasNextFileNumber() const { return has_next_file_number_; }
    uint64_t NextFileNumber() const { return next_file_number_; }

    struct FileMeta {
        int level;
        uint64_t file_number;
        std::string smallest_ikey;
        std::string largest_ikey;
        uint64_t file_size = 0;
    };

    void AddFile(int level, uint64_t file_number,
                 const std::string& smallest, const std::string& largest,
                 uint64_t file_size) {
        added_files_.push_back({level, file_number, smallest, largest, file_size});
    }
    void DeleteFile(int level, uint64_t file_number) {
        deleted_files_.push_back({level, file_number});
    }

    const std::vector<FileMeta>& AddedFiles()   const { return added_files_; }
    const std::vector<std::pair<int, uint64_t>>& DeletedFiles() const { return deleted_files_; }

    std::string Encode() const;
    bool Decode(const char* data, size_t size);

private:
    uint64_t last_sequence_ = 0;
    bool     has_last_sequence_ = false;
    uint64_t log_number_ = 0;
    bool     has_log_number_ = false;
    uint64_t next_file_number_ = 0;
    bool     has_next_file_number_ = false;
    std::vector<FileMeta> added_files_;
    std::vector<std::pair<int, uint64_t>> deleted_files_;
};

} // namespace lsm
