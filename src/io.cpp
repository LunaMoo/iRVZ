// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-3.0-or-later

#include "io.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace irvz {

#ifdef _WIN32
static std::wstring Widen(const std::string& s) {
	if (s.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}
#endif

std::FILE* OpenFile(const std::string& utf8_path, const char* mode) {
#ifdef _WIN32
	return _wfopen(Widen(utf8_path).c_str(), Widen(mode).c_str());
#else
	return std::fopen(utf8_path.c_str(), mode);
#endif
}

bool SeekFile(std::FILE* file, u64 offset) {
#ifdef _WIN32
	return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
	return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

static bool FileSize(std::FILE* file, u64* size) {
#ifdef _WIN32
	if (_fseeki64(file, 0, SEEK_END) != 0)
		return false;
	const long long pos = _ftelli64(file);
#else
	if (fseeko(file, 0, SEEK_END) != 0)
		return false;
	const off_t pos = ftello(file);
#endif
	if (pos < 0)
		return false;
	*size = static_cast<u64>(pos);
	return true;
}

void RemoveFile(const std::string& utf8_path) {
#ifdef _WIN32
	_wremove(Widen(utf8_path).c_str());
#else
	std::remove(utf8_path.c_str());
#endif
}

std::unique_ptr<FileSource> FileSource::Open(const std::string& utf8_path) {
	std::FILE* file = OpenFile(utf8_path, "rb");
	if (!file)
		return nullptr;
	auto source = std::unique_ptr<FileSource>(new FileSource);
	source->m_file = file;
	if (!FileSize(file, &source->m_size))
		return nullptr;
	return source;
}

FileSource::~FileSource() {
	if (m_file)
		std::fclose(m_file);
}

bool FileSource::ReadAt(u64 offset, void* buf, std::size_t len) {
	if (offset > m_size || len > m_size - offset)
		return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	return SeekFile(m_file, offset) && std::fread(buf, 1, len, m_file) == len;
}

std::unique_ptr<FileSink> FileSink::Create(const std::string& utf8_path) {
	std::FILE* file = OpenFile(utf8_path, "wb");
	if (!file)
		return nullptr;
	auto sink = std::unique_ptr<FileSink>(new FileSink);
	sink->m_file = file;
	sink->m_position = 0;
	return sink;
}

FileSink::~FileSink() {
	if (m_file)
		std::fclose(m_file);
}

bool FileSink::WriteAt(u64 offset, const void* data, std::size_t len) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (offset != m_position && !SeekFile(m_file, offset))
		return false;
	m_position = UINT64_MAX;
	if (std::fwrite(data, 1, len, m_file) != len)
		return false;
	m_position = offset + len;
	return true;
}

bool FileSink::Finish(u64 final_size) {
	std::lock_guard<std::mutex> lock(m_mutex);
	const bool ok = std::fclose(m_file) == 0;
	m_file = nullptr;
	return ok;
}

}  // namespace irvz
