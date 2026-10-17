// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where bytes come from and go to: files, memory or the caller's callbacks. Everything above this
// layer works the same whether an image lives on disk or in RAM.
#pragma once

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "common.h"
#include "disc.h"

namespace irvz {

// Random-access input. ReadAt must be safe to call from several threads at once.
class Source {
public:
	virtual ~Source() = default;
	virtual u64 Size() const = 0;
	virtual bool ReadAt(u64 offset, void* buf, std::size_t len) = 0;
};

// Random-access output.
class Sink {
public:
	virtual ~Sink() = default;
	// size is the final size when known (extraction), 0 when it isn't (compression).
	virtual bool Begin(u64 size) { return true; }
	virtual bool WriteAt(u64 offset, const void* data, std::size_t len) = 0;
	virtual bool Finish(u64 final_size) { return true; }
};

// fopen() that takes UTF-8 paths on every platform.
std::FILE* OpenFile(const std::string& utf8_path, const char* mode);
bool SeekFile(std::FILE* file, u64 offset);
void RemoveFile(const std::string& utf8_path);

class FileSource final : public Source {
public:
	static std::unique_ptr<FileSource> Open(const std::string& utf8_path);
	~FileSource() override;
	u64 Size() const override { return m_size; }
	bool ReadAt(u64 offset, void* buf, std::size_t len) override;

private:
	std::FILE* m_file = nullptr;
	u64 m_size = 0;
	std::mutex m_mutex;
};

class MemorySource final : public Source {
public:
	MemorySource(const void* data, u64 size) : m_data(static_cast<const u8*>(data)), m_size(size) {}
	u64 Size() const override { return m_size; }
	bool ReadAt(u64 offset, void* buf, std::size_t len) override {
		if (offset > m_size || len > m_size - offset)
			return false;
		if (len)
			std::memcpy(buf, m_data + offset, len);
		return true;
	}

private:
	const u8* m_data;
	u64 m_size;
};

class FileSink final : public Sink {
public:
	static std::unique_ptr<FileSink> Create(const std::string& utf8_path);
	~FileSink() override;
	bool WriteAt(u64 offset, const void* data, std::size_t len) override;
	bool Finish(u64 final_size) override;

private:
	std::FILE* m_file = nullptr;
	u64 m_position = UINT64_MAX;
	std::mutex m_mutex;
};

// A plain disc image as a BlobReader.
class PlainReader final : public BlobReader {
public:
	explicit PlainReader(std::shared_ptr<Source> source) : m_source(std::move(source)) {}
	u64 GetDataSize() const override { return m_source->Size(); }
	bool Read(u64 offset, u64 size, u8* out) override {
		return size == static_cast<std::size_t>(size) && m_source->ReadAt(offset, out, static_cast<std::size_t>(size));
	}

private:
	std::shared_ptr<Source> m_source;
};

}  // namespace irvz
