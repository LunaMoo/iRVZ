// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-3.0-or-later
//
// irvz: command line front end.
//   irvz info <image.rvz|image.wia>
//   irvz compress [options] <input> <output>    ISO/GCM, WIA or RVZ in; WIA or RVZ out
//   irvz extract [-t threads] [--ram] <input> <output.iso>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "irvz/irvz.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::FILE* Open(const char* path, const char* mode) {
#ifdef _WIN32
	auto widen = [](const char* s) {
		const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
		std::wstring w(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
		return w;
	};
	return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
	return std::fopen(path, mode);
#endif
}

bool LoadFile(const char* path, std::vector<unsigned char>* data) {
	std::FILE* f = Open(path, "rb");
	if (!f)
		return false;
	std::vector<unsigned char> buf(1 << 20);
	size_t n;
	while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
		data->insert(data->end(), buf.begin(), buf.begin() + n);
	const bool ok = !std::ferror(f);
	std::fclose(f);
	return ok;
}

bool SaveFile(const char* path, const irvz_buffer& buffer) {
	std::FILE* f = Open(path, "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(buffer.data, 1, buffer.size, f) == buffer.size;
	return std::fclose(f) == 0 && ok;
}

int last_percent = -1;
int Progress(void*, uint64_t done, uint64_t total) {
	const int percent = total ? static_cast<int>(done * 100 / total) : 100;
	if (percent != last_percent) {
		last_percent = percent;
		std::fprintf(stderr, "\r%3d%%", percent);
		std::fflush(stderr);
	}
	return 0;
}

int Report(int rc, double seconds) {
	std::fprintf(stderr, "\n");
	if (rc != IRVZ_OK) {
		std::fprintf(stderr, "error: %s (%s)\n", irvz_error_string(rc), irvz_last_error());
		return 1;
	}
	std::fprintf(stderr, "done in %.2f s\n", seconds);
	return 0;
}

bool ParseCompression(const char* s, irvz_compression* out) {
	for (int c = IRVZ_COMPRESSION_NONE; c <= IRVZ_COMPRESSION_ZSTD; ++c) {
		if (std::strcmp(s, irvz_compression_name(static_cast<irvz_compression>(c))) == 0) {
			*out = static_cast<irvz_compression>(c);
			return true;
		}
	}
	return false;
}

uint32_t ParseSize(const char* s) {
	char* end = nullptr;
	unsigned long long v = std::strtoull(s, &end, 10);
	if (end && (*end == 'k' || *end == 'K'))
		v *= 1024;
	else if (end && (*end == 'm' || *end == 'M'))
		v *= 1024 * 1024;
	return static_cast<uint32_t>(v);
}

int Usage() {
	std::fprintf(stderr,
	             "iRVZ %s - WIA/RVZ disc image tool\n\n"
	             "usage:\n"
	             "  irvz info <image>\n"
	             "  irvz compress [options] <input> <output>\n"
	             "      -f rvz|wia        output format (default rvz)\n"
	             "      -c method         none, purge (WIA), bzip2, lzma, lzma2, zstd (RVZ)\n"
	             "                        (default zstd for RVZ, lzma2 for WIA)\n"
	             "      -l level          compression level (default 5)\n"
	             "      -b size           chunk size, e.g. 128K or 2M (default 128K RVZ, 2M WIA)\n"
	             "      -t threads        0 = one per CPU core (default)\n"
	             "      --ram             load the input into RAM and build the output in RAM\n"
	             "  irvz extract [-t threads] [--ram] <input> <output.iso>\n",
	             irvz_version());
	return 2;
}

int Info(const char* path) {
	irvz_file* file = nullptr;
	int rc = irvz_open(path, &file);
	if (rc != IRVZ_OK) {
		std::fprintf(stderr, "error: %s (%s)\n", irvz_error_string(rc), irvz_last_error());
		return 1;
	}
	irvz_info info;
	irvz_get_info(file, &info);
	irvz_close(file);
	static const char* disc_types[] = {"unknown", "GameCube", "Wii"};
	std::printf("format:      %s %u.%02x\n", info.format == IRVZ_FORMAT_RVZ ? "RVZ" : "WIA", info.version >> 24,
	            (info.version >> 16) & 0xFF);
	std::printf("disc:        %s, %s \"%s\"\n", info.disc_type < 3 ? disc_types[info.disc_type] : "?", info.game_id,
	            info.title);
	std::printf("compression: %s, level %d, chunk %u KiB\n", irvz_compression_name(info.compression),
	            info.compression_level, info.chunk_size / 1024);
	std::printf("size:        %llu -> %llu bytes (%.1f%%)\n", static_cast<unsigned long long>(info.iso_size),
	            static_cast<unsigned long long>(info.file_size),
	            info.iso_size ? 100.0 * static_cast<double>(info.file_size) / static_cast<double>(info.iso_size) : 0.0);
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3)
		return Usage();
	const std::string command = argv[1];
	if (command == "info")
		return Info(argv[2]);

	irvz_compress_options options;
	irvz_compress_options_init(&options, IRVZ_FORMAT_RVZ);
	bool format_set = false, compression_set = false, chunk_set = false, ram = false;
	int threads = 0;
	std::vector<const char*> paths;
	for (int i = 2; i < argc; ++i) {
		const std::string arg = argv[i];
		const bool has_value = i + 1 < argc;
		if (arg == "-f" && has_value) {
			const std::string f = argv[++i];
			if (f != "rvz" && f != "wia")
				return Usage();
			const irvz_compress_options keep = options;
			irvz_compress_options_init(&options, f == "wia" ? IRVZ_FORMAT_WIA : IRVZ_FORMAT_RVZ);
			options.level = keep.level;
			options.threads = keep.threads;
			if (compression_set)
				options.compression = keep.compression;
			if (chunk_set)
				options.chunk_size = keep.chunk_size;
			format_set = true;
		} else if (arg == "-c" && has_value) {
			if (!ParseCompression(argv[++i], &options.compression))
				return Usage();
			compression_set = true;
		} else if (arg == "-l" && has_value) {
			options.level = std::atoi(argv[++i]);
		} else if (arg == "-b" && has_value) {
			options.chunk_size = ParseSize(argv[++i]);
			chunk_set = true;
		} else if (arg == "-t" && has_value) {
			threads = std::atoi(argv[++i]);
			options.threads = threads;
		} else if (arg == "--ram") {
			ram = true;
		} else if (!arg.empty() && arg[0] == '-') {
			return Usage();
		} else {
			paths.push_back(argv[i]);
		}
	}
	(void)format_set;
	if (paths.size() != 2)
		return Usage();

	const auto start = std::chrono::steady_clock::now();
	const auto elapsed = [&] {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	};

	int rc;
	if (command == "compress" || command == "extract") {
		if (ram) {
			std::vector<unsigned char> input;
			if (!LoadFile(paths[0], &input)) {
				std::fprintf(stderr, "error: can't read %s\n", paths[0]);
				return 1;
			}
			irvz_buffer output{};
			rc = command == "compress" ?
			         irvz_compress_memory(input.data(), input.size(), &output, &options, Progress, nullptr) :
			         irvz_extract_memory(input.data(), input.size(), &output, threads, Progress, nullptr);
			if (rc == IRVZ_OK && !SaveFile(paths[1], output)) {
				std::fprintf(stderr, "\nerror: can't write %s\n", paths[1]);
				irvz_buffer_free(&output);
				return 1;
			}
			irvz_buffer_free(&output);
		} else if (command == "compress") {
			rc = irvz_compress_file(paths[0], paths[1], &options, Progress, nullptr);
		} else {
			rc = irvz_extract_file(paths[0], paths[1], threads, Progress, nullptr);
		}
	} else {
		return Usage();
	}
	return Report(rc, elapsed());
}
