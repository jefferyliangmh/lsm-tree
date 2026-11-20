#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsm/env.h"

namespace test {

enum class FaultPoint {
	kAppend,
	kAppendAt,
	kSync,
	kRead,
	kDeleteFile,
	kRenameFile,
	kSyncDir
};

class FaultPlan {
public:
	void Arm(FaultPoint point, std::string path_fragment) {
		std::lock_guard lock(mu_);
		point_ = point;
		path_fragment_ = std::move(path_fragment);
		armed_ = true;
	}

	bool Consume(FaultPoint point, const std::string& path, lsm::Status* status) {
		std::lock_guard lock(mu_);
		if (!armed_ || point_ != point || path.find(path_fragment_) == std::string::npos)
			return false;
		armed_ = false;
		*status = lsm::Status::IOError("injected filesystem failure: " + path);
		return true;
	}

	bool IsArmed() const {
		std::lock_guard lock(mu_);
		return armed_;
	}

private:
	mutable std::mutex mu_;
	FaultPoint point_ = FaultPoint::kAppend;
	std::string path_fragment_;
	bool armed_ = false;
};

class FaultInjectingFileSystem final : public lsm::FileSystem {
public:
	FaultInjectingFileSystem(lsm::FileSystem* base, std::shared_ptr<FaultPlan> plan)
		: base_(base), plan_(std::move(plan)) {}

	lsm::Status NewWritableFile(const std::string& path,
		std::unique_ptr<lsm::WritableFile>* result) override {
		return OpenWritable(path, false, result);
	}

	lsm::Status NewAppendableFile(const std::string& path,
		std::unique_ptr<lsm::WritableFile>* result) override {
		return OpenWritable(path, true, result);
	}

	lsm::Status NewRandomAccessFile(const std::string& path,
		std::unique_ptr<lsm::RandomAccessFile>* result) override {
		std::unique_ptr<lsm::RandomAccessFile> file;
		auto status = base_->NewRandomAccessFile(path, &file);
		if (!status.ok()) return status;
		*result = std::make_unique<RandomAccessFile>(std::move(file), path, plan_);
		return lsm::Status::OK();
	}

	lsm::Status FileExists(const std::string& path) override {
		return base_->FileExists(path);
	}
	lsm::Status GetFileSize(const std::string& path, uint64_t* size) override {
		return base_->GetFileSize(path, size);
	}
	lsm::Status DeleteFile(const std::string& path) override {
		lsm::Status status;
		if (plan_->Consume(FaultPoint::kDeleteFile, path, &status)) return status;
		return base_->DeleteFile(path);
	}
	lsm::Status TruncateFile(const std::string& path, uint64_t size) override {
		return base_->TruncateFile(path, size);
	}
	lsm::Status RenameFile(const std::string& src, const std::string& dst) override {
		lsm::Status status;
		if (plan_->Consume(FaultPoint::kRenameFile, src + " -> " + dst, &status))
			return status;
		return base_->RenameFile(src, dst);
	}
	lsm::Status CreateDirIfMissing(const std::string& path) override {
		return base_->CreateDirIfMissing(path);
	}
	lsm::Status GetChildren(const std::string& dir,
		std::vector<std::string>* result) override {
		return base_->GetChildren(dir, result);
	}
	lsm::Status SyncDir(const std::string& dir) override {
		lsm::Status status;
		if (plan_->Consume(FaultPoint::kSyncDir, dir, &status)) return status;
		return base_->SyncDir(dir);
	}

private:
	class WritableFile final : public lsm::WritableFile {
	public:
		WritableFile(std::unique_ptr<lsm::WritableFile> file, std::string path,
			std::shared_ptr<FaultPlan> plan)
			: file_(std::move(file)), path_(std::move(path)), plan_(std::move(plan)) {}

		lsm::Status Append(const lsm::Slice& data) override {
			return FailOr(FaultPoint::kAppend, [&] { return file_->Append(data); });
		}
		lsm::Status AppendAt(const lsm::Slice& data, uint64_t offset) override {
			return FailOr(FaultPoint::kAppendAt,
				[&] { return file_->AppendAt(data, offset); });
		}
		lsm::Status Flush() override { return file_->Flush(); }
		lsm::Status Sync() override {
			return FailOr(FaultPoint::kSync, [&] { return file_->Sync(); });
		}
		uint64_t Size() const override { return file_->Size(); }

	private:
		template<class F>
		lsm::Status FailOr(FaultPoint point, F&& operation) {
			lsm::Status status;
			if (plan_->Consume(point, path_, &status)) return status;
			return operation();
		}

		std::unique_ptr<lsm::WritableFile> file_;
		std::string path_;
		std::shared_ptr<FaultPlan> plan_;
	};

	class RandomAccessFile final : public lsm::RandomAccessFile {
	public:
		RandomAccessFile(std::unique_ptr<lsm::RandomAccessFile> file,
			std::string path, std::shared_ptr<FaultPlan> plan)
			: file_(std::move(file)), path_(std::move(path)), plan_(std::move(plan)) {}

		lsm::Status Read(uint64_t offset, size_t n, lsm::Slice* result,
			char* scratch) const override {
			lsm::Status status;
			if (plan_->Consume(FaultPoint::kRead, path_, &status)) return status;
			return file_->Read(offset, n, result, scratch);
		}
		uint64_t Size() const override { return file_->Size(); }
		void* NativeHandle() const override { return file_->NativeHandle(); }

	private:
		std::unique_ptr<lsm::RandomAccessFile> file_;
		std::string path_;
		std::shared_ptr<FaultPlan> plan_;
	};

	lsm::Status OpenWritable(const std::string& path, bool append,
		std::unique_ptr<lsm::WritableFile>* result) {
		std::unique_ptr<lsm::WritableFile> file;
		auto status = append ? base_->NewAppendableFile(path, &file)
							 : base_->NewWritableFile(path, &file);
		if (!status.ok()) return status;
		*result = std::make_unique<WritableFile>(std::move(file), path, plan_);
		return lsm::Status::OK();
	}

	lsm::FileSystem* base_;
	std::shared_ptr<FaultPlan> plan_;
};

class ScopedFileSystemOverride {
public:
	explicit ScopedFileSystemOverride(lsm::FileSystem* file_system)
		: previous_(lsm::FileSystem::SetDefaultForTesting(file_system)) {}
	~ScopedFileSystemOverride() {
		lsm::FileSystem::SetDefaultForTesting(previous_);
	}

	ScopedFileSystemOverride(const ScopedFileSystemOverride&) = delete;
	ScopedFileSystemOverride& operator=(const ScopedFileSystemOverride&) = delete;

private:
	lsm::FileSystem* previous_;
};

} // namespace test
