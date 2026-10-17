// iRVZ - WIA/RVZ disc image library
// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The codecs of WIA and RVZ chunks (none, Purge, bzip2, LZMA, LZMA2, Zstandard) and RVZ packing,
// behind streaming Decompressor / Compressor interfaces. Ported from Dolphin's WIACompression.h,
// with liblzma replaced by the LZMA SDK (the same streams, a smaller library).
#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "common.h"

namespace irvz {

enum class CompressionType : u32 {
	None = 0,
	Purge = 1,
	Bzip2 = 2,
	LZMA = 3,
	LZMA2 = 4,
	Zstd = 5,
};

// {min, max}; {0, -1} when the method has no levels.
std::pair<int, int> GetAllowedCompressionLevels(CompressionType type);

struct DecompressionBuffer {
	std::vector<u8> data;
	std::size_t bytes_written = 0;
};

struct PurgeSegment {
	u32 offset;
	u32 size;
};
static_assert(sizeof(PurgeSegment) == 0x08, "Wrong size for WIA purge segment");

class Decompressor {
public:
	virtual ~Decompressor() = default;
	virtual bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) = 0;
	virtual bool Done() const { return m_done; }

protected:
	bool m_done = false;
};

// compressor_data is header 2's compressor data (LZMA properties).
// max_output bounds how much the stream will produce (it caps LZMA dictionary allocations).
// purge_size is the decompressed size Purge must produce.
std::unique_ptr<Decompressor> CreateDecompressor(CompressionType type, const u8* compressor_data,
                                                 std::size_t compressor_data_size, u64 max_output,
                                                 u64 purge_size);

// Undoes RVZ packing on top of another decompressor (see docs/format.md).
std::unique_ptr<Decompressor> CreateRVZPackDecompressor(std::unique_ptr<Decompressor> decompressor,
                                                        DecompressionBuffer decompressed, u64 data_offset,
                                                        u32 rvz_packed_size);

class Compressor {
public:
	virtual ~Compressor() = default;
	// Start, then AddPrecedingDataOnlyForPurgeHashing / Compress any number of times, then End,
	// then GetData / GetSize.
	virtual bool Start(u64 size) = 0;
	virtual bool AddPrecedingDataOnlyForPurgeHashing(const u8* data, std::size_t size) { return true; }
	virtual bool Compress(const u8* data, std::size_t size) = 0;
	virtual bool End() = 0;
	virtual const u8* GetData() const = 0;
	virtual std::size_t GetSize() const = 0;
};

// Null for CompressionType::None. When compressor_data_out is given, the header 2 compressor data
// for this method and level is written there (up to 7 bytes) and its size to *compressor_data_size_out.
std::unique_ptr<Compressor> CreateCompressor(CompressionType type, int level, u8* compressor_data_out = nullptr,
                                             u8* compressor_data_size_out = nullptr);

}  // namespace irvz
