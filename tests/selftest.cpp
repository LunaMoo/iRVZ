// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Round-trip self-test. Builds small synthetic GameCube and Wii discs (with a file system, junk
// data the way Nintendo's tools made it, and for Wii an encrypted, hashed partition with a broken
// hash so hash exceptions get exercised), then compresses them with every format / method / chunk
// size combination and checks that decompression gives back the exact bytes - through memory,
// callbacks, files, random-access reads and multithreaded extraction.

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "irvz/irvz.h"

#include "crypto.h"
#include "disc.h"
#include "lfg.h"

using namespace irvz;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                   \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAILED: %s (%s:%d) %s\n", #cond, __FILE__, __LINE__, irvz_last_error()); \
			++g_failures;                                                             \
			return false;                                                             \
		}                                                                             \
	} while (0)

void Put32(std::vector<u8>& v, std::size_t offset, u32 x) {
	v[offset] = static_cast<u8>(x >> 24);
	v[offset + 1] = static_cast<u8>(x >> 16);
	v[offset + 2] = static_cast<u8>(x >> 8);
	v[offset + 3] = static_cast<u8>(x);
}

// Junk like Nintendo's mastering tools: LFG output restarted at every 32 KiB block.
void FillJunk(std::vector<u8>& v, std::size_t begin, std::size_t end, u64 offset_base, std::mt19937& rng) {
	for (std::size_t pos = begin; pos < end;) {
		const u64 disc_offset = offset_base + pos;
		const u64 block_end = (disc_offset / 0x8000 + 1) * 0x8000;
		const std::size_t n = static_cast<std::size_t>(std::min<u64>(block_end - disc_offset, end - pos));
		u32 seed[LaggedFibonacciGenerator::SEED_SIZE];
		for (u32& s : seed)
			s = rng();
		LaggedFibonacciGenerator lfg;
		lfg.SetSeed(seed);
		lfg.Forward(static_cast<std::size_t>(disc_offset % 0x8000));
		lfg.GetBytes(n, v.data() + pos);
		pos += n;
	}
}

// A file system with a few files at the given offsets; the rest is junk. offset_shift 2 for Wii.
void BuildFileSystem(std::vector<u8>& d, u32 magic_offset, u32 magic, int offset_shift, std::mt19937& rng) {
	const std::size_t fst_offset = 0x10000;
	struct F {
		u32 offset, size;
		int kind;  // 0 random, 1 zeroes, 2 text
	} files[] = {{0x20000, 0x12345, 0}, {0x40000, 0x30000, 1}, {0x80000, 0x8001, 2}, {0x100000, 0x200000, 0},
	             {0x340000, 0x7000, 0}};
	const std::size_t entries = 1 + sizeof(files) / sizeof(files[0]);

	FillJunk(d, 0x440, d.size(), 0, rng);
	std::memset(d.data(), 0, 0x440);
	std::memcpy(d.data(), "RTST01", 6);
	std::memcpy(d.data() + 0x20, "iRVZ self-test disc", 19);
	Put32(d, magic_offset, magic);
	Put32(d, 0x424, static_cast<u32>(fst_offset >> offset_shift));
	const std::size_t fst_size = entries * 12 + entries * 8;
	Put32(d, 0x428, static_cast<u32>(fst_size >> offset_shift));

	std::vector<u8> fst(fst_size, 0);
	Put32(fst, 0, 0x01000000);
	Put32(fst, 8, static_cast<u32>(entries));
	for (std::size_t i = 0; i < entries - 1; ++i) {
		Put32(fst, (i + 1) * 12, static_cast<u32>(i * 8));
		Put32(fst, (i + 1) * 12 + 4, files[i].offset >> offset_shift);
		Put32(fst, (i + 1) * 12 + 8, files[i].size);
		std::snprintf(reinterpret_cast<char*>(fst.data()) + entries * 12 + i * 8, 8, "f%zu", i);
	}
	std::memcpy(d.data() + fst_offset, fst.data(), fst.size());

	for (const F& f : files) {
		u8* p = d.data() + f.offset;
		if (f.kind == 0)
			for (u32 i = 0; i < f.size; ++i)
				p[i] = static_cast<u8>(rng());
		else if (f.kind == 1)
			std::memset(p, 0, f.size);
		else
			for (u32 i = 0; i < f.size; ++i)
				p[i] = "the quick brown fox jumps over the lazy dog "[i % 44];
		// Files end with up to 32 zero bytes before junk resumes
		const std::size_t pad = std::min<std::size_t>(0x1F - (f.size + 0x1F) % 0x20, d.size() - f.offset - f.size);
		std::memset(p + f.size, 0, pad);
	}
}

std::vector<u8> BuildGameCubeDisc() {
	std::mt19937 rng(1234);
	std::vector<u8> d(0x600000 + 0x1234);  // deliberately not a multiple of 32 KiB
	BuildFileSystem(d, 0x1C, 0xC2339F3D, 0, rng);
	return d;
}

std::vector<u8> BuildWiiDisc() {
	std::mt19937 rng(5678);
	constexpr u64 partition_offset = 0x50000;
	constexpr u64 data_offset = 0x20000;  // relative to the partition
	constexpr u64 groups = 3;
	const u64 data_size = groups * Wii::GROUP_TOTAL_SIZE - 5 * Wii::BLOCK_TOTAL_SIZE;  // a partial last group
	std::vector<u8> d(partition_offset + data_offset + data_size + 0x48000, 0);

	FillJunk(d, 0, d.size(), 0, rng);
	std::memset(d.data(), 0, 0x80);
	std::memcpy(d.data(), "RTSW01", 6);
	std::memcpy(d.data() + 0x20, "iRVZ self-test Wii disc", 23);
	Put32(d, 0x18, 0x5D1C9EA3);
	std::memset(d.data() + 0x40000, 0, 0x100);
	Put32(d, 0x40000, 1);
	Put32(d, 0x40004, 0x40020 >> 2);
	Put32(d, 0x40020, static_cast<u32>(partition_offset >> 2));
	Put32(d, 0x40024, 0);

	// Ticket: RSA-2048 signature type, title key encrypted with the common key
	std::memset(d.data() + partition_offset, 0, data_offset);
	Put32(d, partition_offset, 0x10001);
	std::memcpy(d.data() + partition_offset + 0x140, "Root-CA00000001-XS00000003", 26);
	const u8 title_id[8] = {0, 1, 0, 0, 'R', 'T', 'S', 'W'};
	std::memcpy(d.data() + partition_offset + 0x1DC, title_id, 8);
	u8 title_key[16];
	for (u8& b : title_key)
		b = static_cast<u8>(rng());
	static const u8 common_key[16] = {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
	                                  0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7};
	u8 iv[16] = {};
	std::memcpy(iv, title_id, 8);
	AES128(common_key, AES128::Mode::Encrypt).Crypt(iv, title_key, d.data() + partition_offset + 0x1BF, 16);
	Put32(d, partition_offset + 0x2B8, static_cast<u32>(data_offset >> 2));
	Put32(d, partition_offset + 0x2BC, static_cast<u32>(data_size >> 2));

	// Partition contents (decrypted), then hash and encrypt them group by group
	const u64 blocks = data_size / Wii::BLOCK_TOTAL_SIZE;
	std::vector<u8> plain(groups * Wii::GROUP_DATA_SIZE, 0);
	BuildFileSystem(plain, 0x18, 0x5D1C9EA3, 2, rng);
	// Junk inside the partition is aligned to the decrypted address space; redo it with that base
	std::memset(plain.data() + blocks * Wii::BLOCK_DATA_SIZE, 0, plain.size() - blocks * Wii::BLOCK_DATA_SIZE);
	// One block of all-same data, to exercise group reuse
	std::memset(plain.data() + 0x500000, 0x55, Wii::BLOCK_DATA_SIZE * 2);

	const AES128 aes(title_key, AES128::Mode::Encrypt);
	std::vector<Wii::BlockData> group_data(Wii::BLOCKS_PER_GROUP);
	std::vector<Wii::HashBlock> hashes(Wii::BLOCKS_PER_GROUP);
	for (u64 g = 0; g < groups; ++g) {
		for (u32 b = 0; b < Wii::BLOCKS_PER_GROUP; ++b)
			std::memcpy(group_data[b].data(), plain.data() + (g * Wii::BLOCKS_PER_GROUP + b) * Wii::BLOCK_DATA_SIZE,
			            Wii::BLOCK_DATA_SIZE);
		Wii::HashGroup(group_data.data(), hashes.data());
		if (g == 1)
			hashes[3].h0[7][0] ^= 0xFF;  // a bad hash: has to survive as a hash exception
		if (g == 2)
			hashes[0].padding_1[5] = 0x42;  // nonzero padding, likewise
		for (u32 b = 0; b < Wii::BLOCKS_PER_GROUP; ++b) {
			const u64 block = g * Wii::BLOCKS_PER_GROUP + b;
			if (block >= blocks)
				break;
			Wii::EncryptBlock(hashes[b], group_data[b].data(),
			                  d.data() + partition_offset + data_offset + block * Wii::BLOCK_TOTAL_SIZE, aes);
		}
	}
	return d;
}

struct Case {
	irvz_format format;
	irvz_compression compression;
	int level;
	u32 chunk;
};

bool RoundTrip(const char* name, const std::vector<u8>& disc, const Case& c) {
	irvz_compress_options o;
	irvz_compress_options_init(&o, c.format);
	o.compression = c.compression;
	o.level = c.level;
	o.chunk_size = c.chunk;
	o.threads = 3;

	irvz_buffer packed{};
	int rc = irvz_compress_memory(disc.data(), disc.size(), &packed, &o, nullptr, nullptr);
	CHECK(rc == IRVZ_OK);
	CHECK(irvz_detect(packed.data, packed.size) == c.format);

	irvz_file* f = nullptr;
	CHECK(irvz_open_memory(packed.data, packed.size, &f) == IRVZ_OK);
	irvz_info info;
	CHECK(irvz_get_info(f, &info) == IRVZ_OK);
	CHECK(info.iso_size == disc.size() && info.file_size == packed.size && info.chunk_size == c.chunk);
	CHECK(info.compression == c.compression);

	// Random-access reads
	std::mt19937 rng(42);
	std::vector<u8> buf;
	for (int i = 0; i < 40; ++i) {
		const u64 off = rng() % disc.size();
		const std::size_t len = static_cast<std::size_t>(std::min<u64>(rng() % 0x300000 + 1, disc.size() - off));
		buf.resize(len);
		CHECK(irvz_read(f, off, buf.data(), len) == IRVZ_OK);
		CHECK(std::memcmp(buf.data(), disc.data() + off, len) == 0);
	}
	irvz_close(f);

	// Whole-image extraction, single- and multithreaded
	for (int threads : {1, 4}) {
		irvz_buffer out{};
		CHECK(irvz_extract_memory(packed.data, packed.size, &out, threads, nullptr, nullptr) == IRVZ_OK);
		CHECK(out.size == disc.size() && std::memcmp(out.data, disc.data(), disc.size()) == 0);
		irvz_buffer_free(&out);
	}

	std::printf("ok   %-4s %s %-5s level %2d chunk %5u KiB: %llu -> %llu\n", name,
	            c.format == IRVZ_FORMAT_RVZ ? "RVZ" : "WIA", irvz_compression_name(c.compression), c.level, c.chunk / 1024,
	            static_cast<unsigned long long>(disc.size()), static_cast<unsigned long long>(packed.size));
	irvz_buffer_free(&packed);
	return true;
}

int ReadCallback(void* user, uint64_t offset, void* buf, size_t len) {
	const std::vector<u8>* v = static_cast<const std::vector<u8>*>(user);
	std::memcpy(buf, v->data() + offset, len);
	return 0;
}

int Cancel(void*, uint64_t done, uint64_t) {
	return done > 0 ? 1 : 0;
}

// Conversions between formats, callbacks, files and cancellation.
bool Misc(const std::vector<u8>& disc, const std::string& tmp) {
	// RVZ -> WIA -> plain through callbacks
	irvz_reader r{const_cast<std::vector<u8>*>(&disc), disc.size(), ReadCallback};
	irvz_buffer rvz{};
	irvz_writer w;
	irvz_writer_from_buffer(&w, &rvz);
	CHECK(irvz_compress(&r, &w, nullptr, nullptr, nullptr) == IRVZ_OK);

	irvz_compress_options o;
	irvz_compress_options_init(&o, IRVZ_FORMAT_WIA);
	o.compression = IRVZ_COMPRESSION_BZIP2;
	irvz_buffer wia{};
	CHECK(irvz_compress_memory(rvz.data, rvz.size, &wia, &o, nullptr, nullptr) == IRVZ_OK);
	irvz_buffer plain{};
	CHECK(irvz_extract_memory(wia.data, wia.size, &plain, 0, nullptr, nullptr) == IRVZ_OK);
	CHECK(plain.size == disc.size() && std::memcmp(plain.data, disc.data(), disc.size()) == 0);

	// Files
	const std::string iso = tmp + "_in.iso", packed = tmp + "_out.rvz", back = tmp + "_back.iso";
	std::FILE* fp = std::fopen(iso.c_str(), "wb");
	CHECK(fp && std::fwrite(disc.data(), 1, disc.size(), fp) == disc.size());
	std::fclose(fp);
	CHECK(irvz_compress_file(iso.c_str(), packed.c_str(), nullptr, nullptr, nullptr) == IRVZ_OK);
	CHECK(irvz_extract_file(packed.c_str(), back.c_str(), 2, nullptr, nullptr) == IRVZ_OK);
	std::vector<u8> read_back(disc.size());
	fp = std::fopen(back.c_str(), "rb");
	CHECK(fp && std::fread(read_back.data(), 1, read_back.size(), fp) == read_back.size());
	std::fclose(fp);
	CHECK(read_back == disc);
	irvz_file* f = nullptr;
	CHECK(irvz_open(packed.c_str(), &f) == IRVZ_OK);
	irvz_info info;
	irvz_get_info(f, &info);
	irvz_close(f);
	CHECK(std::strcmp(info.game_id, reinterpret_cast<const char*>(disc.data())) == 0 || info.game_id[0] == disc[0]);

	// Cancellation leaves no output file behind
	CHECK(irvz_compress_file(iso.c_str(), packed.c_str(), nullptr, Cancel, nullptr) == IRVZ_ERR_CANCELLED);
	fp = std::fopen(packed.c_str(), "rb");
	CHECK(!fp);
	CHECK(irvz_extract_memory(rvz.data, rvz.size, &plain, 1, Cancel, nullptr) == IRVZ_ERR_CANCELLED);

	// Invalid input / options
	CHECK(irvz_open_memory(disc.data(), disc.size(), &f) == IRVZ_ERR_FORMAT);
	o.compression = IRVZ_COMPRESSION_ZSTD;  // WIA can't do zstd
	irvz_buffer bad{};
	CHECK(irvz_compress_memory(disc.data(), disc.size(), &bad, &o, nullptr, nullptr) == IRVZ_ERR_ARGS);
	// A corrupted file is rejected or fails to read, never crashes
	std::vector<u8> corrupt(rvz.data, rvz.data + rvz.size);
	corrupt[corrupt.size() / 2] ^= 0x5A;
	if (irvz_open_memory(corrupt.data(), corrupt.size(), &f) == IRVZ_OK) {
		irvz_buffer out{};
		irvz_extract_memory(corrupt.data(), corrupt.size(), &out, 2, nullptr, nullptr);
		irvz_buffer_free(&out);
		irvz_close(f);
	}

	std::remove(iso.c_str());
	std::remove(packed.c_str());
	std::remove(back.c_str());
	irvz_buffer_free(&rvz);
	irvz_buffer_free(&wia);
	irvz_buffer_free(&plain);
	std::printf("ok   conversions, callbacks, files, cancellation, invalid input\n");
	return true;
}

bool CryptoVectors() {
	// FIPS-197 C.1 and SHA-1 "abc"
	const u8 key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	const u8 plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
	const u8 expected[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
	                         0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
	u8 out[16], back[16];
	AES128(key, AES128::Mode::Encrypt).CryptIvZero(plain, out, 16);
	CHECK(std::memcmp(out, expected, 16) == 0);
	AES128(key, AES128::Mode::Decrypt).CryptIvZero(out, back, 16);
	CHECK(std::memcmp(back, plain, 16) == 0);
	const SHA1::Digest d = SHA1::CalculateDigest("abc", 3);
	const u8 sha[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
	                    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
	CHECK(std::memcmp(d.data(), sha, 20) == 0);
	std::printf("ok   AES-128 / SHA-1 test vectors\n");
	return true;
}

}  // namespace

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const std::string tmp = argc > 1 ? argv[1] : "irvz_selftest";
	CryptoVectors();

	const std::vector<u8> gc = BuildGameCubeDisc();
	const std::vector<u8> wii = BuildWiiDisc();

	const Case cases[] = {
	    {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_ZSTD, 5, 128 * 1024},  {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_ZSTD, 19, 32 * 1024},
	    {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_ZSTD, 3, 2 << 20},     {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_ZSTD, 1, 4 << 20},
	    {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_NONE, 0, 64 * 1024},   {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_BZIP2, 9, 512 * 1024},
	    {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_LZMA, 6, 256 * 1024},  {IRVZ_FORMAT_RVZ, IRVZ_COMPRESSION_LZMA2, 9, 1 << 20},
	    {IRVZ_FORMAT_WIA, IRVZ_COMPRESSION_NONE, 0, 2 << 20},     {IRVZ_FORMAT_WIA, IRVZ_COMPRESSION_PURGE, 0, 2 << 20},
	    {IRVZ_FORMAT_WIA, IRVZ_COMPRESSION_BZIP2, 5, 4 << 20},    {IRVZ_FORMAT_WIA, IRVZ_COMPRESSION_LZMA, 5, 2 << 20},
	    {IRVZ_FORMAT_WIA, IRVZ_COMPRESSION_LZMA2, 7, 2 << 20},
	};
	for (const Case& c : cases) {
		RoundTrip("GC", gc, c);
		RoundTrip("Wii", wii, c);
	}
	Misc(gc, tmp + "_gc");
	Misc(wii, tmp + "_wii");

	if (g_failures) {
		std::printf("%d check(s) FAILED\n", g_failures);
		return 1;
	}
	std::printf("all tests passed\n");
	return 0;
}
