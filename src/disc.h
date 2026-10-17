// iRVZ - WIA/RVZ disc image library
// Copyright 2008-2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The few parts of GameCube / Wii disc structure that WIA and RVZ depend on: Wii partitions
// (tickets, title keys, hashed and encrypted 32 KiB blocks) and the file system table that RVZ uses
// to tell junk data from file data. Condensed from Dolphin's Blob.h, Volume*.cpp,
// FileSystemGCWii.cpp and DiscUtils.cpp.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "common.h"
#include "crypto.h"

namespace irvz {

namespace Wii {
constexpr u32 BLOCKS_PER_GROUP = 0x40;
constexpr u64 BLOCK_HEADER_SIZE = 0x0400;
constexpr u64 BLOCK_DATA_SIZE = 0x7C00;
constexpr u64 BLOCK_TOTAL_SIZE = BLOCK_HEADER_SIZE + BLOCK_DATA_SIZE;
constexpr u64 GROUP_HEADER_SIZE = BLOCK_HEADER_SIZE * BLOCKS_PER_GROUP;
constexpr u64 GROUP_DATA_SIZE = BLOCK_DATA_SIZE * BLOCKS_PER_GROUP;
constexpr u64 GROUP_TOTAL_SIZE = GROUP_HEADER_SIZE + GROUP_DATA_SIZE;

using Key = std::array<u8, 16>;
using BlockData = std::array<u8, BLOCK_DATA_SIZE>;

struct HashBlock {
	std::array<SHA1::Digest, 31> h0;
	std::array<u8, 20> padding_0;
	std::array<SHA1::Digest, 8> h1;
	std::array<u8, 32> padding_1;
	std::array<SHA1::Digest, 8> h2;
	std::array<u8, 32> padding_2;
};
static_assert(sizeof(HashBlock) == BLOCK_HEADER_SIZE);

// Fills in the H0/H1/H2 hashes of a 2 MiB group from its 64 blocks of data.
void HashGroup(const BlockData in[BLOCKS_PER_GROUP], HashBlock out[BLOCKS_PER_GROUP]);
void DecryptBlockHashes(const u8* in, HashBlock* out, const AES128& aes);
void DecryptBlockData(const u8* in, u8* out, const AES128& aes);
// Encrypts one block: the hash block with a zero IV, the data with bytes 0x3D0-0x3DF of the
// encrypted hash block as IV.
void EncryptBlock(const HashBlock& hashes, const u8* data, u8* out, const AES128& aes);
}  // namespace Wii

// Random access to the bytes of a disc image (a plain ISO or a decompressing WIA/RVZ reader).
class BlobReader {
public:
	virtual ~BlobReader() = default;
	virtual u64 GetDataSize() const = 0;
	virtual bool Read(u64 offset, u64 size, u8* out) = 0;

	// Lets readers that store Wii partitions decrypted skip the encrypt-then-decrypt round trip.
	virtual bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const { return false; }
	virtual bool ReadWiiDecrypted(u64 offset, u64 size, u8* out, u64 partition_data_offset) { return false; }

	std::optional<u32> ReadSwapped32(u64 offset) {
		u8 b[4];
		if (!Read(offset, 4, b))
			return std::nullopt;
		return swap32(b);
	}
	std::optional<u8> ReadU8(u64 offset) {
		u8 b;
		if (!Read(offset, 1, &b))
			return std::nullopt;
		return b;
	}
};

// GameCube / Wii file system table, reduced to the one query RVZ packing needs.
class FileSystem {
public:
	struct File {
		u64 offset;
		u64 size;
	};
	// fst is the whole FST; offset_shift is 2 on Wii (offsets are stored divided by 4), else 0.
	static std::unique_ptr<FileSystem> Create(std::vector<u8> fst, u8 offset_shift);
	// The file containing disc_offset (in the partition's own address space), if any.
	std::optional<File> FindFileInfo(u64 disc_offset) const;

private:
	std::map<u64, File> m_files_by_end;
};

enum class DiscType : u32 { Unknown = 0, GameCube = 1, Wii = 2 };

// A GameCube or Wii disc read through a BlobReader (which this does not own).
class Volume {
public:
	struct Partition {
		u64 offset = 0;       // of the partition (its ticket) on the disc
		u64 data_offset = 0;  // of its data, relative to offset
		bool ticket_valid = false;
		Wii::Key title_key{};
	};

	// Null when the image is neither a GameCube nor a Wii disc.
	static std::unique_ptr<Volume> Create(BlobReader* reader);

	DiscType GetType() const { return m_type; }
	bool HasWiiHashes() const { return m_has_hashes; }
	bool HasWiiEncryption() const { return m_has_encryption; }
	// Sorted by offset.
	const std::vector<Partition>& GetPartitions() const { return m_partitions; }

	// partition == nullptr reads the disc itself; otherwise the partition's decrypted data.
	bool Read(u64 offset, u64 size, u8* out, const Partition* partition) const;
	std::optional<u32> ReadSwapped32(u64 offset, const Partition* partition) const;
	std::optional<u64> ReadSwappedAndShifted(u64 offset, const Partition* partition) const;
	u64 PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const;
	// The file system of a partition (or of a GameCube disc, with nullptr), or null.
	std::unique_ptr<FileSystem> CreateFileSystem(const Partition* partition) const;

private:
	explicit Volume(BlobReader* reader) : m_reader(reader) {}
	u32 OffsetShift() const { return m_type == DiscType::Wii ? 2 : 0; }

	BlobReader* m_reader;
	DiscType m_type = DiscType::Unknown;
	bool m_has_hashes = false;
	bool m_has_encryption = false;
	std::vector<Partition> m_partitions;

	mutable u64 m_last_decrypted_block = UINT64_MAX;
	mutable std::unique_ptr<u8[]> m_last_decrypted_block_data;
};

}  // namespace irvz
