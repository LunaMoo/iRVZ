// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C interface (include/irvz/irvz.h) on top of the C++ reader and writer.

#include "irvz/irvz.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "compression.h"
#include "io.h"
#include "wia.h"

using namespace irvz;

#define IRVZ_STR2(x) #x
#define IRVZ_STR(x) IRVZ_STR2(x)

struct irvz_file {
	std::shared_ptr<Source> source;
	std::unique_ptr<WIAReader> reader;
};

namespace {

thread_local std::string g_last_error;

int Fail(int code, const std::string& detail) {
	g_last_error = detail;
	return code;
}

int Ok() {
	g_last_error.clear();
	return IRVZ_OK;
}

class CallbackSource final : public Source {
public:
	explicit CallbackSource(const irvz_reader& reader) : m_reader(reader) {}
	u64 Size() const override { return m_reader.size; }
	bool ReadAt(u64 offset, void* buf, std::size_t len) override {
		if (offset > m_reader.size || len > m_reader.size - offset)
			return false;
		return len == 0 || m_reader.read(m_reader.user, offset, buf, len) == 0;
	}

private:
	irvz_reader m_reader;
};

class CallbackSink final : public Sink {
public:
	explicit CallbackSink(const irvz_writer& writer) : m_writer(writer) {}
	bool Begin(u64 size) override { return !m_writer.begin || m_writer.begin(m_writer.user, size) == 0; }
	bool WriteAt(u64 offset, const void* data, std::size_t len) override {
		return m_writer.write(m_writer.user, offset, data, len) == 0;
	}
	bool Finish(u64 final_size) override { return !m_writer.finish || m_writer.finish(m_writer.user, final_size) == 0; }

private:
	irvz_writer m_writer;
};

int MapResult(ConversionResultCode code) {
	switch (code) {
	case ConversionResultCode::Success:
		return Ok();
	case ConversionResultCode::Canceled:
		return Fail(IRVZ_ERR_CANCELLED, "cancelled");
	case ConversionResultCode::ReadFailed:
		return Fail(IRVZ_ERR_READ, "reading the input failed");
	case ConversionResultCode::WriteFailed:
		return Fail(IRVZ_ERR_WRITE, "writing the output failed");
	default:
		return Fail(IRVZ_ERR_INTERNAL, "internal error");
	}
}

int OpenSource(std::shared_ptr<Source> source, irvz_file** out) {
	if (!out)
		return Fail(IRVZ_ERR_ARGS, "no output handle");
	*out = nullptr;
	std::unique_ptr<WIAReader> reader = WIAReader::Create(source);
	if (!reader)
		return Fail(IRVZ_ERR_FORMAT, "not a valid WIA or RVZ file");
	*out = new irvz_file{std::move(source), std::move(reader)};
	return Ok();
}

int Compress(std::shared_ptr<Source> source, Sink* sink, const irvz_compress_options* options,
             irvz_progress progress, void* user) {
	irvz_compress_options defaults;
	if (!options) {
		irvz_compress_options_init(&defaults, IRVZ_FORMAT_RVZ);
		options = &defaults;
	}
	if (options->format != IRVZ_FORMAT_RVZ && options->format != IRVZ_FORMAT_WIA)
		return Fail(IRVZ_ERR_ARGS, "the output format must be WIA or RVZ");

	ConvertOptions convert;
	convert.rvz = options->format == IRVZ_FORMAT_RVZ;
	convert.compression = static_cast<CompressionType>(options->compression);
	convert.compression_level = options->level;
	convert.chunk_size = static_cast<int>(std::min<u32>(options->chunk_size, 0x7FFFFFFF));
	convert.threads = options->threads;
	if (const char* problem = ValidateConvertOptions(convert))
		return Fail(IRVZ_ERR_ARGS, problem);

	// The input may itself be WIA/RVZ (conversion between them, or recompression)
	std::unique_ptr<BlobReader> input = WIAReader::Create(source);
	if (!input)
		input = std::make_unique<PlainReader>(source);

	ProgressCallback callback;
	if (progress)
		callback = [progress, user](u64 done, u64 total) { return progress(user, done, total) == 0; };
	return MapResult(ConvertToWIAOrRVZ(input.get(), sink, convert, callback));
}

int Extract(irvz_file* file, Sink* sink, int threads, irvz_progress progress, void* user) {
	if (!file || !sink)
		return Fail(IRVZ_ERR_ARGS, "missing file or output");

	const u64 iso_size = file->reader->GetDataSize();
	if (!sink->Begin(iso_size))
		return Fail(IRVZ_ERR_WRITE, "the output couldn't be prepared");

	// Pieces that decompress independently, merged into jobs of a few MiB
	const std::vector<u64> points = file->reader->GetSplitPoints();
	constexpr u64 JOB_SIZE = 4 * 1024 * 1024;
	std::vector<std::pair<u64, u64>> jobs;
	for (std::size_t i = 0; i + 1 < points.size(); ++i) {
		if (!jobs.empty() && jobs.back().second == points[i] && points[i + 1] - jobs.back().first <= JOB_SIZE)
			jobs.back().second = points[i + 1];
		else
			jobs.emplace_back(points[i], points[i + 1]);
	}

	unsigned workers = threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency();
	workers = std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(std::max<std::size_t>(1, jobs.size()))));

	std::atomic<std::size_t> next_job{0};
	std::atomic<u64> done_bytes{0};
	std::atomic<bool> stop{false};
	std::atomic<int> error{IRVZ_OK};
	std::atomic<unsigned> running{workers};
	std::mutex mutex;
	std::condition_variable cv;

	const auto work = [&](std::unique_ptr<WIAReader> reader) {
		try {
			std::vector<u8> buffer;
			while (!stop.load()) {
				const std::size_t index = next_job.fetch_add(1);
				if (index >= jobs.size())
					break;
				const u64 start = jobs[index].first, size = jobs[index].second - jobs[index].first;
				buffer.resize(static_cast<std::size_t>(size));
				if (!reader->Read(start, size, buffer.data())) {
					error.store(IRVZ_ERR_READ);
					stop.store(true);
					break;
				}
				if (!sink->WriteAt(start, buffer.data(), buffer.size())) {
					error.store(IRVZ_ERR_WRITE);
					stop.store(true);
					break;
				}
				done_bytes.fetch_add(size);
			}
		} catch (const std::bad_alloc&) {
			error.store(IRVZ_ERR_MEMORY);
			stop.store(true);
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			--running;
		}
		cv.notify_all();
	};

	std::vector<std::thread> pool;
	for (unsigned i = 0; i < workers; ++i)
		pool.emplace_back(work, file->reader->Clone());

	bool cancelled = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		while (running.load() != 0) {
			cv.wait_for(lock, std::chrono::milliseconds(100));
			if (progress && !cancelled) {
				lock.unlock();
				if (progress(user, done_bytes.load(), iso_size) != 0) {
					cancelled = true;
					stop.store(true);
				}
				lock.lock();
			}
		}
	}
	for (std::thread& t : pool)
		t.join();

	if (cancelled)
		return Fail(IRVZ_ERR_CANCELLED, "cancelled");
	switch (error.load()) {
	case IRVZ_ERR_READ:
		return Fail(IRVZ_ERR_READ, "reading or decompressing the image failed");
	case IRVZ_ERR_WRITE:
		return Fail(IRVZ_ERR_WRITE, "writing the output failed");
	case IRVZ_ERR_MEMORY:
		return Fail(IRVZ_ERR_MEMORY, "out of memory");
	default:
		break;
	}
	if (progress && progress(user, iso_size, iso_size) != 0)
		return Fail(IRVZ_ERR_CANCELLED, "cancelled");
	if (!sink->Finish(iso_size))
		return Fail(IRVZ_ERR_WRITE, "finishing the output failed");
	return Ok();
}

// irvz_buffer writer callbacks
bool Reserve(irvz_buffer* b, u64 size) {
	if (size <= b->capacity)
		return true;
	if (size != static_cast<std::size_t>(size))
		return false;
	u64 capacity = std::max<u64>(size, b->capacity + b->capacity / 2);
	void* data = std::realloc(b->data, static_cast<std::size_t>(capacity));
	if (!data) {
		capacity = size;
		data = std::realloc(b->data, static_cast<std::size_t>(capacity));
		if (!data)
			return false;
	}
	b->data = static_cast<uint8_t*>(data);
	b->capacity = capacity;
	return true;
}

int BufferBegin(void* user, uint64_t size) {
	irvz_buffer* b = static_cast<irvz_buffer*>(user);
	if (!Reserve(b, size))
		return 1;
	// Extraction writes the whole image from several threads; its size is known up front, so
	// writes never need to grow the buffer (or touch size) concurrently.
	b->size = std::max(b->size, size);
	return 0;
}

int BufferWrite(void* user, uint64_t offset, const void* buf, size_t len) {
	irvz_buffer* b = static_cast<irvz_buffer*>(user);
	const u64 end = offset + len;
	if (end > b->size) {
		if (!Reserve(b, end))
			return 1;
		if (offset > b->size)
			std::memset(b->data + b->size, 0, static_cast<std::size_t>(offset - b->size));
		b->size = end;
	}
	if (len)
		std::memcpy(b->data + offset, buf, len);
	return 0;
}

int BufferFinish(void* user, uint64_t final_size) {
	irvz_buffer* b = static_cast<irvz_buffer*>(user);
	if (final_size <= b->size)
		b->size = final_size;
	return 0;
}

int MemoryRead(void* user, uint64_t offset, void* buf, size_t len) {
	std::memcpy(buf, static_cast<const u8*>(user) + offset, len);
	return 0;
}

template <typename F>
int Guard(F&& f) {
	try {
		return f();
	} catch (const std::bad_alloc&) {
		return Fail(IRVZ_ERR_MEMORY, "out of memory");
	} catch (...) {
		return Fail(IRVZ_ERR_INTERNAL, "unexpected exception");
	}
}

}  // namespace

extern "C" {

void irvz_reader_from_memory(irvz_reader* reader, const void* data, uint64_t size) {
	reader->user = const_cast<void*>(data);
	reader->size = size;
	reader->read = MemoryRead;
}

void irvz_writer_from_buffer(irvz_writer* writer, irvz_buffer* buffer) {
	writer->user = buffer;
	writer->begin = BufferBegin;
	writer->write = BufferWrite;
	writer->finish = BufferFinish;
}

void irvz_buffer_free(irvz_buffer* buffer) {
	if (!buffer)
		return;
	std::free(buffer->data);
	buffer->data = nullptr;
	buffer->size = buffer->capacity = 0;
}

irvz_format irvz_detect(const void* header, size_t len) {
	if (!header || len < 4)
		return IRVZ_FORMAT_UNKNOWN;
	u32 magic;
	std::memcpy(&magic, header, 4);
	if (magic == WIA_MAGIC)
		return IRVZ_FORMAT_WIA;
	if (magic == RVZ_MAGIC)
		return IRVZ_FORMAT_RVZ;
	return IRVZ_FORMAT_UNKNOWN;
}

int irvz_open(const char* utf8_path, irvz_file** out) {
	return Guard([&] {
		if (!utf8_path)
			return Fail(IRVZ_ERR_ARGS, "no path");
		std::shared_ptr<Source> source = FileSource::Open(utf8_path);
		if (!source)
			return Fail(IRVZ_ERR_OPEN, std::string("can't open ") + utf8_path);
		return OpenSource(std::move(source), out);
	});
}

int irvz_open_reader(const irvz_reader* reader, irvz_file** out) {
	return Guard([&] {
		if (!reader || !reader->read)
			return Fail(IRVZ_ERR_ARGS, "invalid reader");
		return OpenSource(std::make_shared<CallbackSource>(*reader), out);
	});
}

int irvz_open_memory(const void* data, uint64_t size, irvz_file** out) {
	return Guard([&] {
		if (!data && size)
			return Fail(IRVZ_ERR_ARGS, "no data");
		return OpenSource(std::make_shared<MemorySource>(data, size), out);
	});
}

void irvz_close(irvz_file* file) {
	delete file;
}

int irvz_get_info(const irvz_file* file, irvz_info* info) {
	if (!file || !info)
		return Fail(IRVZ_ERR_ARGS, "missing file or info");
	const WIAReader& r = *file->reader;
	std::memset(info, 0, sizeof(*info));
	info->format = r.IsRVZ() ? IRVZ_FORMAT_RVZ : IRVZ_FORMAT_WIA;
	info->compression = static_cast<irvz_compression>(r.GetCompressionType());
	info->compression_level = r.GetCompressionLevel();
	info->chunk_size = r.GetChunkSize();
	info->iso_size = r.GetDataSize();
	info->file_size = r.GetFileSize();
	info->disc_type = r.GetDiscType();
	info->version = r.GetVersion();
	info->version_compatible = r.GetVersionCompatible();
	const u8* header = r.GetDiscHeader();
	for (int i = 0; i < 6; ++i)
		info->game_id[i] = header[i] >= 0x20 && header[i] < 0x7F ? static_cast<char>(header[i]) : '\0';
	std::memcpy(info->title, header + 0x20, 64);
	info->title[64] = '\0';
	return Ok();
}

int irvz_read(irvz_file* file, uint64_t offset, void* buf, size_t len) {
	return Guard([&] {
		if (!file || (!buf && len))
			return Fail(IRVZ_ERR_ARGS, "missing file or buffer");
		if (!file->reader->Read(offset, len, static_cast<u8*>(buf)))
			return Fail(IRVZ_ERR_READ, "read failed (out of range or corrupt data)");
		return Ok();
	});
}

void irvz_compress_options_init(irvz_compress_options* options, irvz_format format) {
	options->format = format == IRVZ_FORMAT_WIA ? IRVZ_FORMAT_WIA : IRVZ_FORMAT_RVZ;
	if (options->format == IRVZ_FORMAT_RVZ) {
		options->compression = IRVZ_COMPRESSION_ZSTD;
		options->chunk_size = 128 * 1024;
	} else {
		options->compression = IRVZ_COMPRESSION_LZMA2;
		options->chunk_size = 2 * 1024 * 1024;
	}
	options->level = 5;
	options->threads = 0;
}

int irvz_compress(const irvz_reader* src, const irvz_writer* dst, const irvz_compress_options* options,
                  irvz_progress progress, void* user) {
	return Guard([&] {
		if (!src || !src->read || !dst || !dst->write)
			return Fail(IRVZ_ERR_ARGS, "invalid reader or writer");
		CallbackSink sink(*dst);
		return Compress(std::make_shared<CallbackSource>(*src), &sink, options, progress, user);
	});
}

int irvz_compress_file(const char* src_path, const char* dst_path, const irvz_compress_options* options,
                       irvz_progress progress, void* user) {
	return Guard([&] {
		if (!src_path || !dst_path)
			return Fail(IRVZ_ERR_ARGS, "missing path");
		std::shared_ptr<Source> source = FileSource::Open(src_path);
		if (!source)
			return Fail(IRVZ_ERR_OPEN, std::string("can't open ") + src_path);
		std::unique_ptr<FileSink> sink = FileSink::Create(dst_path);
		if (!sink)
			return Fail(IRVZ_ERR_OPEN, std::string("can't create ") + dst_path);
		const int rc = Compress(std::move(source), sink.get(), options, progress, user);
		if (rc != IRVZ_OK) {
			const std::string detail = g_last_error;
			sink.reset();
			RemoveFile(dst_path);
			g_last_error = detail;
		}
		return rc;
	});
}

int irvz_compress_memory(const void* src, uint64_t size, irvz_buffer* out, const irvz_compress_options* options,
                         irvz_progress progress, void* user) {
	return Guard([&] {
		if ((!src && size) || !out)
			return Fail(IRVZ_ERR_ARGS, "missing input or output buffer");
		irvz_writer writer;
		irvz_writer_from_buffer(&writer, out);
		CallbackSink sink(writer);
		return Compress(std::make_shared<MemorySource>(src, size), &sink, options, progress, user);
	});
}

int irvz_extract(irvz_file* file, const irvz_writer* dst, int threads, irvz_progress progress, void* user) {
	return Guard([&] {
		if (!dst || !dst->write)
			return Fail(IRVZ_ERR_ARGS, "invalid writer");
		CallbackSink sink(*dst);
		return Extract(file, &sink, threads, progress, user);
	});
}

int irvz_extract_file(const char* src_path, const char* dst_path, int threads, irvz_progress progress, void* user) {
	return Guard([&] {
		if (!src_path || !dst_path)
			return Fail(IRVZ_ERR_ARGS, "missing path");
		irvz_file* file = nullptr;
		int rc = irvz_open(src_path, &file);
		if (rc != IRVZ_OK)
			return rc;
		std::unique_ptr<irvz_file> owner(file);
		std::unique_ptr<FileSink> sink = FileSink::Create(dst_path);
		if (!sink)
			return Fail(IRVZ_ERR_OPEN, std::string("can't create ") + dst_path);
		rc = Extract(file, sink.get(), threads, progress, user);
		if (rc != IRVZ_OK) {
			const std::string detail = g_last_error;
			sink.reset();
			RemoveFile(dst_path);
			g_last_error = detail;
		}
		return rc;
	});
}

int irvz_extract_memory(const void* src, uint64_t size, irvz_buffer* out, int threads, irvz_progress progress,
                        void* user) {
	return Guard([&] {
		if (!out)
			return Fail(IRVZ_ERR_ARGS, "no output buffer");
		irvz_file* file = nullptr;
		int rc = irvz_open_memory(src, size, &file);
		if (rc != IRVZ_OK)
			return rc;
		std::unique_ptr<irvz_file> owner(file);
		irvz_writer writer;
		irvz_writer_from_buffer(&writer, out);
		CallbackSink sink(writer);
		return Extract(file, &sink, threads, progress, user);
	});
}

const char* irvz_version(void) {
	return IRVZ_STR(IRVZ_VERSION_MAJOR) "." IRVZ_STR(IRVZ_VERSION_MINOR) "." IRVZ_STR(IRVZ_VERSION_PATCH);
}

const char* irvz_error_string(int code) {
	switch (code) {
	case IRVZ_OK:
		return "success";
	case IRVZ_ERR_CANCELLED:
		return "cancelled";
	case IRVZ_ERR_OPEN:
		return "can't open file";
	case IRVZ_ERR_READ:
		return "read error";
	case IRVZ_ERR_WRITE:
		return "write error";
	case IRVZ_ERR_FORMAT:
		return "not a valid WIA/RVZ file";
	case IRVZ_ERR_ARGS:
		return "invalid arguments";
	case IRVZ_ERR_MEMORY:
		return "out of memory";
	default:
		return "internal error";
	}
}

const char* irvz_last_error(void) {
	return g_last_error.c_str();
}

const char* irvz_compression_name(irvz_compression compression) {
	switch (compression) {
	case IRVZ_COMPRESSION_NONE:
		return "none";
	case IRVZ_COMPRESSION_PURGE:
		return "purge";
	case IRVZ_COMPRESSION_BZIP2:
		return "bzip2";
	case IRVZ_COMPRESSION_LZMA:
		return "lzma";
	case IRVZ_COMPRESSION_LZMA2:
		return "lzma2";
	case IRVZ_COMPRESSION_ZSTD:
		return "zstd";
	}
	return "unknown";
}

void irvz_compression_levels(irvz_compression compression, int* min, int* max) {
	const auto [lo, hi] = GetAllowedCompressionLevels(static_cast<CompressionType>(compression));
	if (min)
		*min = lo;
	if (max)
		*max = hi;
}

}  // extern "C"
