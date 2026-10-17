// iRVZ - WIA/RVZ disc image library
// Copyright 2008-2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disc.h"

#include <algorithm>
#include <cstring>

namespace irvz {

namespace {

constexpr u32 WII_DISC_MAGIC = 0x5D1C9EA3;
constexpr u32 GAMECUBE_DISC_MAGIC = 0xC2339F3D;

// Ticket layout (v0, RSA-2048 signed, as on discs).
constexpr std::size_t TICKET_SIZE = 0x2A4;
constexpr std::size_t TICKET_ISSUER = 0x140;
constexpr std::size_t TICKET_VERSION = 0x1BC;
constexpr std::size_t TICKET_TITLE_KEY = 0x1BF;
constexpr std::size_t TICKET_TITLE_ID = 0x1DC;
constexpr std::size_t TICKET_COMMON_KEY_INDEX = 0x1F1;

// The Wii common keys (retail, Korean) and the development (RVT) common key, as hard-coded in
// Dolphin's IOS emulation (Core/IOS/IOSC.cpp). Title keys in tickets are encrypted with these.
constexpr u8 COMMON_KEY[16] = {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
                               0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7};
constexpr u8 RVT_COMMON_KEY[16] = {0xa1, 0x60, 0x4a, 0x6a, 0x71, 0x23, 0xb5, 0x29,
                                   0xae, 0x8b, 0xec, 0x32, 0xc8, 0x16, 0xfc, 0xaa};
constexpr u8 NEW_COMMON_KEY[16] = {0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e,
                                   0x13, 0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e};

bool TicketIsValid(const u8* ticket) {
	const u32 signature_type = swap32(ticket);
	if (signature_type < 0x10000 || signature_type > 0x10002)
		return false;
	// A v1 ticket is longer than what was read from the disc, which Dolphin treats as invalid.
	return ticket[TICKET_VERSION] != 1;
}

Wii::Key DecryptTitleKey(const u8* ticket) {
	u8 iv[16] = {};
	std::memcpy(iv, ticket + TICKET_TITLE_ID, 8);
	u8 index = ticket[TICKET_COMMON_KEY_INDEX];
	if (index > 1)
		index = 0;
	const bool rvt = std::memcmp(ticket + TICKET_ISSUER, "Root-CA00000002-XS00000006", 27) == 0;
	const u8* common_key = index == 1 ? NEW_COMMON_KEY : rvt ? RVT_COMMON_KEY : COMMON_KEY;
	Wii::Key key;
	AES128(common_key, AES128::Mode::Decrypt).Crypt(iv, ticket + TICKET_TITLE_KEY, key.data(), key.size());
	return key;
}

}  // namespace

// ---- Wii blocks -------------------------------------------------------------------------------

namespace Wii {

void HashGroup(const BlockData in[BLOCKS_PER_GROUP], HashBlock out[BLOCKS_PER_GROUP]) {
	for (std::size_t i = 0; i < BLOCKS_PER_GROUP; ++i) {
		const std::size_t h1_base = AlignDown(i, 8);
		for (std::size_t j = 0; j < 31; ++j)
			out[i].h0[j] = SHA1::CalculateDigest(in[i].data() + j * 0x400, 0x400);
		out[i].padding_0 = {};
		out[h1_base].h1[i - h1_base] = SHA1::CalculateDigest(out[i].h0.data(), sizeof(out[i].h0));

		if (i % 8 == 7) {
			out[h1_base].padding_1 = {};
			for (std::size_t j = 1; j < 8; ++j)
				out[h1_base + j].h1 = out[h1_base].h1;
			out[0].h2[h1_base / 8] = SHA1::CalculateDigest(out[i].h1.data(), sizeof(out[i].h1));
		}
	}
	out[0].padding_2 = {};
	for (std::size_t j = 1; j < BLOCKS_PER_GROUP; ++j)
		out[j].h2 = out[0].h2;
	for (std::size_t j = 1; j < BLOCKS_PER_GROUP; ++j) {
		out[j].padding_1 = {};
		out[j].padding_2 = {};
	}
}

void DecryptBlockHashes(const u8* in, HashBlock* out, const AES128& aes) {
	aes.CryptIvZero(in, reinterpret_cast<u8*>(out), sizeof(HashBlock));
}

void DecryptBlockData(const u8* in, u8* out, const AES128& aes) {
	aes.Crypt(&in[0x3D0], &in[sizeof(HashBlock)], out, BLOCK_DATA_SIZE);
}

void EncryptBlock(const HashBlock& hashes, const u8* data, u8* out, const AES128& aes) {
	aes.CryptIvZero(reinterpret_cast<const u8*>(&hashes), out, BLOCK_HEADER_SIZE);
	aes.Crypt(out + 0x3D0, data, out + BLOCK_HEADER_SIZE, BLOCK_DATA_SIZE);
}

}  // namespace Wii

// ---- File system ------------------------------------------------------------------------------

std::unique_ptr<FileSystem> FileSystem::Create(std::vector<u8> fst, u8 offset_shift) {
	constexpr std::size_t ENTRY_SIZE = 12;
	if (fst.size() < ENTRY_SIZE || fst.back() != 0)
		return nullptr;
	const bool root_is_directory = (swap32(fst.data()) & 0xFF000000) != 0;
	const u32 entries = swap32(fst.data() + 8);
	if (!root_is_directory || static_cast<u64>(entries) * ENTRY_SIZE > fst.size())
		return nullptr;

	auto fs = std::unique_ptr<FileSystem>(new FileSystem);
	for (u32 i = 0; i < entries; ++i) {
		const u8* entry = fst.data() + i * ENTRY_SIZE;
		if (swap32(entry) & 0xFF000000)
			continue;  // directory
		const u32 size = swap32(entry + 8);
		if (size == 0)
			continue;
		const u64 offset = static_cast<u64>(swap32(entry + 4)) << offset_shift;
		fs->m_files_by_end.emplace(offset + size, File{offset, size});
	}
	return fs;
}

std::optional<FileSystem::File> FileSystem::FindFileInfo(u64 disc_offset) const {
	// The first file that ends after disc_offset, if it doesn't start after it either
	const auto it = m_files_by_end.upper_bound(disc_offset);
	if (it == m_files_by_end.end() || it->second.offset > disc_offset)
		return std::nullopt;
	return it->second;
}

// ---- Volume -----------------------------------------------------------------------------------

std::unique_ptr<Volume> Volume::Create(BlobReader* reader) {
	auto volume = std::unique_ptr<Volume>(new Volume(reader));
	if (reader->ReadSwapped32(0x18) == WII_DISC_MAGIC)
		volume->m_type = DiscType::Wii;
	else if (reader->ReadSwapped32(0x1C) == GAMECUBE_DISC_MAGIC)
		volume->m_type = DiscType::GameCube;
	else
		return nullptr;

	if (volume->m_type != DiscType::Wii)
		return volume;

	volume->m_has_hashes = reader->ReadU8(0x60) == u8(0);
	volume->m_has_encryption = reader->ReadU8(0x61) == u8(0);

	std::map<u64, Partition> partitions;
	for (u32 partition_group = 0; partition_group < 4; ++partition_group) {
		const std::optional<u32> number_of_partitions = reader->ReadSwapped32(0x40000 + partition_group * 8);
		const std::optional<u64> table_offset =
		    volume->ReadSwappedAndShifted(0x40000 + partition_group * 8 + 4, nullptr);
		if (!number_of_partitions || !table_offset)
			continue;

		for (u32 i = 0; i < *number_of_partitions; i++) {
			const std::optional<u64> partition_offset = volume->ReadSwappedAndShifted(*table_offset + i * 8, nullptr);
			if (!partition_offset)
				continue;
			// Dolphin skips entries whose type can't be read
			if (!reader->ReadSwapped32(*table_offset + i * 8 + 4))
				continue;
			if (partitions.count(*partition_offset))
				continue;

			Partition partition;
			partition.offset = *partition_offset;
			partition.data_offset = volume->ReadSwappedAndShifted(partition.offset + 0x2B8, nullptr).value_or(0);

			std::vector<u8> ticket(TICKET_SIZE);
			if (reader->Read(partition.offset, ticket.size(), ticket.data()) && TicketIsValid(ticket.data())) {
				partition.ticket_valid = true;
				partition.title_key = DecryptTitleKey(ticket.data());
			}
			partitions.emplace(partition.offset, partition);
		}
	}
	for (const auto& [offset, partition] : partitions)
		volume->m_partitions.push_back(partition);
	return volume;
}

bool Volume::Read(u64 offset, u64 size, u8* out, const Partition* partition) const {
	if (!partition)
		return m_reader->Read(offset, size, out);

	const u64 partition_data_offset = partition->offset + partition->data_offset;
	if (m_has_hashes && m_has_encryption && m_reader->SupportsReadWiiDecrypted(offset, size, partition_data_offset))
		return m_reader->ReadWiiDecrypted(offset, size, out, partition_data_offset);

	if (!m_has_hashes)
		return m_reader->Read(partition_data_offset + offset, size, out);

	std::unique_ptr<AES128> aes;
	std::unique_ptr<u8[]> read_buffer;
	if (m_has_encryption) {
		if (!partition->ticket_valid)
			return false;
		aes = std::make_unique<AES128>(partition->title_key.data(), AES128::Mode::Decrypt);
		read_buffer = std::make_unique<u8[]>(Wii::BLOCK_TOTAL_SIZE);
	}
	if (!m_last_decrypted_block_data)
		m_last_decrypted_block_data = std::make_unique<u8[]>(Wii::BLOCK_DATA_SIZE);

	while (size > 0) {
		const u64 block_offset_on_disc =
		    partition_data_offset + offset / Wii::BLOCK_DATA_SIZE * Wii::BLOCK_TOTAL_SIZE;
		const u64 data_offset_in_block = offset % Wii::BLOCK_DATA_SIZE;

		if (m_last_decrypted_block != block_offset_on_disc) {
			m_last_decrypted_block = UINT64_MAX;
			if (m_has_encryption) {
				if (!m_reader->Read(block_offset_on_disc, Wii::BLOCK_TOTAL_SIZE, read_buffer.get()))
					return false;
				Wii::DecryptBlockData(read_buffer.get(), m_last_decrypted_block_data.get(), *aes);
			} else if (!m_reader->Read(block_offset_on_disc + Wii::BLOCK_HEADER_SIZE, Wii::BLOCK_DATA_SIZE,
			                           m_last_decrypted_block_data.get())) {
				return false;
			}
			m_last_decrypted_block = block_offset_on_disc;
		}

		const u64 copy_size = std::min(size, Wii::BLOCK_DATA_SIZE - data_offset_in_block);
		std::memcpy(out, m_last_decrypted_block_data.get() + data_offset_in_block, static_cast<std::size_t>(copy_size));
		size -= copy_size;
		out += copy_size;
		offset += copy_size;
	}
	return true;
}

std::optional<u32> Volume::ReadSwapped32(u64 offset, const Partition* partition) const {
	u8 b[4];
	if (!Read(offset, 4, b, partition))
		return std::nullopt;
	return swap32(b);
}

std::optional<u64> Volume::ReadSwappedAndShifted(u64 offset, const Partition* partition) const {
	const std::optional<u32> value = ReadSwapped32(offset, partition);
	if (!value)
		return std::nullopt;
	return static_cast<u64>(*value) << OffsetShift();
}

u64 Volume::PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const {
	if (!m_has_hashes)
		return partition.offset + partition.data_offset + offset;
	return partition.offset + partition.data_offset + offset / Wii::BLOCK_DATA_SIZE * Wii::BLOCK_TOTAL_SIZE +
	       offset % Wii::BLOCK_DATA_SIZE;
}

std::unique_ptr<FileSystem> Volume::CreateFileSystem(const Partition* partition) const {
	u8 offset_shift;
	if (ReadSwapped32(0x18, partition) == WII_DISC_MAGIC)
		offset_shift = 2;
	else if (ReadSwapped32(0x1C, partition) == GAMECUBE_DISC_MAGIC)
		offset_shift = 0;
	else
		return nullptr;

	const std::optional<u64> fst_offset = ReadSwappedAndShifted(0x424, partition);
	const std::optional<u64> fst_size = ReadSwappedAndShifted(0x428, partition);
	// No real file system comes anywhere near 128 MiB (more than a Wii's RAM)
	if (!fst_offset || !fst_size || *fst_size > 128 * 1024 * 1024)
		return nullptr;

	std::vector<u8> fst(static_cast<std::size_t>(*fst_size));
	if (!Read(*fst_offset, fst.size(), fst.data(), partition))
		return nullptr;
	return FileSystem::Create(std::move(fst), offset_shift);
}

}  // namespace irvz
