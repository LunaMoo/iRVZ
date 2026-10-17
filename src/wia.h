// iRVZ - WIA/RVZ disc image library
// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading and writing WIA and RVZ images. Ported from Dolphin's Source/Core/DiscIO/WIABlob.h.
// The format is described in docs/format.md.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "common.h"
#include "compression.h"
#include "disc.h"
#include "io.h"

namespace irvz {

constexpr u32 WIA_MAGIC = 0x01414957;  // "WIA\x1" (byteswapped to little endian)
constexpr u32 RVZ_MAGIC = 0x015A5652;  // "RVZ\x1" (byteswapped to little endian)

// A decompressing reader: Read() returns the bytes of the original disc image.
class WIAReader : public BlobReader {
public:
	// Null if the source isn't a valid WIA or RVZ file.
	static std::unique_ptr<WIAReader> Create(std::shared_ptr<Source> source);

	// Another reader over the same source, for use on another thread.
	virtual std::unique_ptr<WIAReader> Clone() const = 0;

	virtual bool IsRVZ() const = 0;
	virtual CompressionType GetCompressionType() const = 0;
	virtual int GetCompressionLevel() const = 0;
	virtual u32 GetChunkSize() const = 0;
	virtual u64 GetFileSize() const = 0;
	virtual u32 GetDiscType() const = 0;
	virtual u32 GetVersion() const = 0;
	virtual u32 GetVersionCompatible() const = 0;
	virtual const u8* GetDiscHeader() const = 0;  // first 0x80 bytes of the disc

	// Offsets at which the image can be cut into pieces that decompress independently (each piece
	// covers whole chunks and whole Wii hash groups), sorted, from 0 to GetDataSize().
	virtual std::vector<u64> GetSplitPoints() const = 0;
};

enum class ConversionResultCode {
	Success,
	Canceled,
	ReadFailed,
	WriteFailed,
	InternalError,
};

struct ConvertOptions {
	bool rvz = true;
	CompressionType compression = CompressionType::Zstd;
	int compression_level = 5;
	int chunk_size = 128 * 1024;
	int threads = 0;  // 0 = one per CPU core
};

// done / total in input bytes. Returning false cancels. Called on the converting thread.
using ProgressCallback = std::function<bool(u64 done, u64 total)>;

// Checks chunk size, compression method and level for the format. Empty if they're fine.
const char* ValidateConvertOptions(const ConvertOptions& options);

ConversionResultCode ConvertToWIAOrRVZ(BlobReader* infile, Sink* outfile, const ConvertOptions& options,
                                       const ProgressCallback& progress);

}  // namespace irvz
