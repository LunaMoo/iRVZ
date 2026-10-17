// iRVZ - WIA/RVZ disc image library
// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Dolphin's Source/Core/DiscIO/WIABlob.cpp, WiiEncryptionCache.cpp and
// MultithreadedCompressor.h. The logic is Dolphin's, so output matches Dolphin's for the same
// settings (apart from LZMA streams, which come from a different encoder); I/O goes through
// Source / Sink so images can live in files or in RAM.

#include "wia.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <zstd.h>

#include "lfg.h"

namespace irvz {

namespace {

void PushBack(std::vector<u8>* vector, const u8* begin, const u8* end) {
	const std::size_t offset_in_vector = vector->size();
	vector->resize(offset_in_vector + (end - begin));
	std::copy(begin, end, vector->data() + offset_in_vector);
}

template <typename T>
void PushBack(std::vector<u8>* vector, const T& x) {
	static_assert(std::is_trivially_copyable_v<T>);
	const u8* x_ptr = reinterpret_cast<const u8*>(&x);
	PushBack(vector, x_ptr, x_ptr + sizeof(T));
}

bool AllAre(const u8* begin, const u8* end, u8 x) {
	return std::all_of(begin, end, [x](u8 y) { return x == y; });
}
bool AllZero(const std::vector<u8>& data) {
	return AllAre(data.data(), data.data() + data.size(), 0);
}
bool AllSame(const std::vector<u8>& data) {
	return AllAre(data.data(), data.data() + data.size(), data.front());
}
bool AllSame(const u8* begin, const u8* end) {
	return AllAre(begin, end, *begin);
}

// Dolphin's Common::Event: an auto-resetting flag.
class Event {
public:
	void Set() {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_flag = true;
		}
		m_cv.notify_one();
	}
	void Wait() {
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cv.wait(lock, [this] { return m_flag; });
		m_flag = false;
	}
	void Reset() {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_flag = false;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_flag = false;
};

template <typename T>
struct ConversionResult {
	ConversionResultCode code = ConversionResultCode::Success;
	T value{};
};

// Starts a number of compression threads and one output thread. The output thread handles data in
// the order it was submitted with CompressAndWrite; the compression threads in no particular order.
// (Dolphin's MultithreadedCompressor.)
template <typename CompressThreadState, typename CompressParameters, typename OutputParameters>
class MultithreadedCompressor {
public:
	MultithreadedCompressor(
	    std::function<ConversionResultCode(CompressThreadState*)> set_up_compress_thread_state,
	    std::function<ConversionResult<OutputParameters>(CompressThreadState*, CompressParameters)> compress,
	    std::function<ConversionResultCode(OutputParameters)> output, unsigned threads)
	    : m_set_up_compress_thread_state(std::move(set_up_compress_thread_state)), m_compress(std::move(compress)),
	      m_output(std::move(output)), m_threads(std::max<unsigned>(1, threads)) {
		m_compress_threads = std::make_unique<CompressThread[]>(m_threads);
		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].thread = std::thread(&MultithreadedCompressor::CompressThreadFunction, this, &m_compress_threads[i]);
		m_output_thread = std::thread(&MultithreadedCompressor::OutputThreadFunction, this);
	}

	~MultithreadedCompressor() {
		if (!m_shutting_down.load())
			Shutdown();
	}

	void CompressAndWrite(CompressParameters parameters) {
		if (GetStatus() != ConversionResultCode::Success)
			return;

		CompressThread& compress_thread = m_compress_threads[m_current_index];
		compress_thread.compress_ready_event.Wait();
		compress_thread.compress_parameters = std::move(parameters);
		compress_thread.compress_event.Set();

		++m_current_index;
		if (m_current_index >= m_threads)
			m_current_index -= m_threads;
	}

	void SetError(ConversionResultCode result) {
		// If we already have an error, don't overwrite it
		ConversionResultCode expected = ConversionResultCode::Success;
		m_result.compare_exchange_strong(expected, result);
	}

	ConversionResultCode GetStatus() const { return m_result.load(); }

	void Shutdown() {
		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].compress_ready_event.Wait();
		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].compress_done_event.Wait();
		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].output_ready_event.Wait();

		m_shutting_down.store(true);

		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].compress_event.Set();
		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].output_event.Set();

		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].thread.join();
		m_output_thread.join();
	}

private:
	struct CompressThread {
		std::thread thread;
		Event compress_ready_event;
		Event compress_event;
		Event compress_done_event;
		Event output_ready_event;
		Event output_event;
		CompressParameters compress_parameters;
		OutputParameters output_parameters;
	};

	void CompressThreadFunction(CompressThread* state) {
		CompressThreadState compress_thread_state;

		const ConversionResultCode setup_result = m_set_up_compress_thread_state(&compress_thread_state);
		if (setup_result != ConversionResultCode::Success)
			SetError(setup_result);

		state->compress_ready_event.Set();
		state->compress_done_event.Set();

		while (true) {
			state->compress_event.Wait();
			if (m_shutting_down.load())
				return;

			CompressParameters parameters = std::move(state->compress_parameters);
			state->compress_done_event.Reset();
			state->compress_ready_event.Set();

			ConversionResult<OutputParameters> result = m_compress(&compress_thread_state, std::move(parameters));
			if (result.code == ConversionResultCode::Success) {
				state->output_ready_event.Wait();
				state->output_parameters = std::move(result.value);
				state->output_event.Set();
			} else {
				SetError(result.code);
			}

			state->compress_done_event.Set();
		}
	}

	void OutputThreadFunction() {
		for (std::size_t i = 0; i < m_threads; ++i)
			m_compress_threads[i].output_ready_event.Set();

		std::size_t index = 0;
		while (true) {
			CompressThread& compress_thread = m_compress_threads[index];
			compress_thread.output_event.Wait();
			if (m_shutting_down.load())
				return;

			OutputParameters parameters = std::move(compress_thread.output_parameters);
			compress_thread.output_ready_event.Set();

			// Once something failed, the remaining output is dropped
			if (GetStatus() == ConversionResultCode::Success) {
				const ConversionResultCode result = m_output(std::move(parameters));
				if (result != ConversionResultCode::Success)
					SetError(result);
			}

			++index;
			if (index >= m_threads)
				index -= m_threads;
		}
	}

	std::function<ConversionResultCode(CompressThreadState*)> m_set_up_compress_thread_state;
	std::function<ConversionResult<OutputParameters>(CompressThreadState*, CompressParameters)> m_compress;
	std::function<ConversionResultCode(OutputParameters)> m_output;

	std::unique_ptr<CompressThread[]> m_compress_threads;
	std::thread m_output_thread;

	const std::size_t m_threads;
	std::size_t m_current_index = 0;

	std::atomic<ConversionResultCode> m_result{ConversionResultCode::Success};
	std::atomic<bool> m_shutting_down{false};
};

using WiiKey = Wii::Key;

// ---- On-disk structures -----------------------------------------------------------------------

#pragma pack(push, 1)
struct WIAHeader1 {
	u32 magic;
	u32 version;
	u32 version_compatible;
	u32 header_2_size;
	SHA1::Digest header_2_hash;
	u64 iso_file_size;
	u64 wia_file_size;
	SHA1::Digest header_1_hash;
};
static_assert(sizeof(WIAHeader1) == 0x48, "Wrong size for WIA header 1");

struct WIAHeader2 {
	u32 disc_type;
	u32 compression_type;
	s32 compression_level;  // Informative only
	u32 chunk_size;

	std::array<u8, 0x80> disc_header;

	u32 number_of_partition_entries;
	u32 partition_entry_size;
	u64 partition_entries_offset;
	SHA1::Digest partition_entries_hash;

	u32 number_of_raw_data_entries;
	u64 raw_data_entries_offset;
	u32 raw_data_entries_size;

	u32 number_of_group_entries;
	u64 group_entries_offset;
	u32 group_entries_size;

	u8 compressor_data_size;
	u8 compressor_data[7];
};
static_assert(sizeof(WIAHeader2) == 0xdc, "Wrong size for WIA header 2");

struct PartitionDataEntry {
	u32 first_sector;
	u32 number_of_sectors;
	u32 group_index;
	u32 number_of_groups;
};
static_assert(sizeof(PartitionDataEntry) == 0x10, "Wrong size for WIA partition data entry");

struct PartitionEntry {
	WiiKey partition_key;
	std::array<PartitionDataEntry, 2> data_entries;
};
static_assert(sizeof(PartitionEntry) == 0x30, "Wrong size for WIA partition entry");

struct RawDataEntry {
	u64 data_offset;
	u64 data_size;
	u32 group_index;
	u32 number_of_groups;
};
static_assert(sizeof(RawDataEntry) == 0x18, "Wrong size for WIA raw data entry");

struct WIAGroupEntry {
	u32 data_offset;  // >> 2
	u32 data_size;
};
static_assert(sizeof(WIAGroupEntry) == 0x08, "Wrong size for WIA group entry");

struct RVZGroupEntry {
	u32 data_offset;  // >> 2
	u32 data_size;
	u32 rvz_packed_size;
};
static_assert(sizeof(RVZGroupEntry) == 0x0c, "Wrong size for RVZ group entry");

struct HashExceptionEntry {
	u16 offset;
	SHA1::Digest hash;
};
static_assert(sizeof(HashExceptionEntry) == 0x16, "Wrong size for WIA hash exception entry");
#pragma pack(pop)

struct DataEntry {
	u32 index;
	bool is_partition;
	u8 partition_data_index;

	DataEntry(std::size_t index_) : index(static_cast<u32>(index_)), is_partition(false), partition_data_index(0) {}
	DataEntry(std::size_t index_, std::size_t partition_data_index_)
	    : index(static_cast<u32>(index_)), is_partition(true),
	      partition_data_index(static_cast<u8>(partition_data_index_)) {}
};

// Perhaps WIA_VERSION_WRITE_COMPATIBLE could be 0.9, but WIA version 0.9 was never in any official
// release of wit, so Dolphin sets it to 1.0 to be safe.
constexpr u32 WIA_VERSION = 0x01000000;
constexpr u32 WIA_VERSION_WRITE_COMPATIBLE = 0x01000000;
constexpr u32 WIA_VERSION_READ_COMPATIBLE = 0x00080000;

constexpr u32 RVZ_VERSION = 0x01000000;
constexpr u32 RVZ_VERSION_WRITE_COMPATIBLE = 0x00030000;
constexpr u32 RVZ_VERSION_READ_COMPATIBLE = 0x00030000;

// ---- Reader -----------------------------------------------------------------------------------

template <bool RVZ>
class WIARVZFileReader final : public WIAReader {
public:
	using GroupEntry = std::conditional_t<RVZ, RVZGroupEntry, WIAGroupEntry>;

	explicit WIARVZFileReader(std::shared_ptr<Source> source) : m_source(std::move(source)) { m_valid = Initialize(); }

	bool IsValid() const { return m_valid; }

	std::unique_ptr<WIAReader> Clone() const override {
		auto copy = std::unique_ptr<WIARVZFileReader>(new WIARVZFileReader());
		copy->m_valid = m_valid;
		copy->m_compression_type = m_compression_type;
		copy->m_source = m_source;
		copy->m_header_1 = m_header_1;
		copy->m_header_2 = m_header_2;
		copy->m_partition_entries = m_partition_entries;
		copy->m_raw_data_entries = m_raw_data_entries;
		copy->m_group_entries = m_group_entries;
		copy->m_data_entries = m_data_entries;
		return copy;
	}

	bool IsRVZ() const override { return RVZ; }
	CompressionType GetCompressionType() const override { return m_compression_type; }
	int GetCompressionLevel() const override { return static_cast<s32>(swap32(static_cast<u32>(m_header_2.compression_level))); }
	u32 GetChunkSize() const override { return swap32(m_header_2.chunk_size); }
	u64 GetFileSize() const override { return swap64(m_header_1.wia_file_size); }
	u64 GetDataSize() const override { return swap64(m_header_1.iso_file_size); }
	u32 GetDiscType() const override { return swap32(m_header_2.disc_type); }
	u32 GetVersion() const override { return swap32(m_header_1.version); }
	u32 GetVersionCompatible() const override { return swap32(m_header_1.version_compatible); }
	const u8* GetDiscHeader() const override { return m_header_2.disc_header.data(); }
	std::vector<u64> GetSplitPoints() const override;

	bool Read(u64 offset, u64 size, u8* out_ptr) override;
	bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override;
	bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) override;

private:
	class Chunk {
	public:
		Chunk() = default;
		Chunk(Source* source, u64 offset_in_file, u64 compressed_size, u64 decompressed_size, u32 exception_lists,
		      bool compressed_exception_lists, u32 rvz_packed_size, u64 data_offset,
		      std::unique_ptr<Decompressor> decompressor);

		bool Read(u64 offset, u64 size, u8* out_ptr);

		// This can only be called once at least one byte of data has been read
		void GetHashExceptions(std::vector<HashExceptionEntry>* exception_list, u64 exception_list_index,
		                       u16 additional_offset) const;

		template <typename T>
		bool ReadAll(std::vector<T>* vector) {
			return Read(0, vector->size() * sizeof(T), reinterpret_cast<u8*>(vector->data()));
		}

	private:
		bool Decompress();
		bool HandleExceptions(const u8* data, std::size_t bytes_allocated, std::size_t bytes_written,
		                      std::size_t* bytes_used, bool align);
		std::size_t GetOutBytesWrittenExcludingExceptions() const;

		DecompressionBuffer m_in;
		DecompressionBuffer m_out;
		std::size_t m_in_bytes_read = 0;

		std::unique_ptr<Decompressor> m_decompressor = nullptr;
		Source* m_source = nullptr;
		u64 m_offset_in_file = 0;

		std::size_t m_out_bytes_allocated_for_exceptions = 0;
		std::size_t m_out_bytes_used_for_exceptions = 0;
		std::size_t m_in_bytes_used_for_exceptions = 0;
		u32 m_exception_lists = 0;
		bool m_compressed_exception_lists = false;
		u32 m_rvz_packed_size = 0;
		u64 m_data_offset = 0;
	};

	WIARVZFileReader() = default;
	bool Initialize();
	bool HasDataOverlap() const;

	const PartitionEntry* GetPartition(u64 partition_data_offset, u32* partition_first_sector) const;

	bool ReadFromGroups(u64* offset, u64* size, u8** out_ptr, u64 chunk_size, u32 sector_size, u64 data_offset,
	                    u64 data_size, u32 group_index, u32 number_of_groups, u32 exception_lists);
	Chunk& ReadCompressedData(u64 offset_in_file, u64 compressed_size, u64 decompressed_size,
	                          CompressionType compression_type, u32 exception_lists = 0, u32 rvz_packed_size = 0,
	                          u64 data_offset = 0);

	static bool ApplyHashExceptions(const std::vector<HashExceptionEntry>& exception_list,
	                                Wii::HashBlock hash_blocks[Wii::BLOCKS_PER_GROUP]);

	// WiiEncryptionCache: re-creates the encrypted, hashed Wii groups from the decrypted data.
	const std::array<u8, Wii::GROUP_TOTAL_SIZE>* EncryptGroup(u64 offset, u64 partition_data_offset,
	                                                         u64 partition_data_decrypted_size, const WiiKey& key,
	                                                         bool* hash_exception_error);
	bool EncryptGroups(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset,
	                   u64 partition_data_decrypted_size, const WiiKey& key, bool* hash_exception_error);

	bool m_valid = false;
	CompressionType m_compression_type = CompressionType::None;

	std::shared_ptr<Source> m_source;
	Chunk m_cached_chunk;
	u64 m_cached_chunk_offset = std::numeric_limits<u64>::max();

	std::unique_ptr<std::array<u8, Wii::GROUP_TOTAL_SIZE>> m_encryption_cache;
	u64 m_encryption_cache_offset = std::numeric_limits<u64>::max();

	std::vector<HashExceptionEntry> m_exception_list;
	bool m_write_to_exception_list = false;
	u64 m_exception_list_last_group_index = 0;

	WIAHeader1 m_header_1{};
	WIAHeader2 m_header_2{};
	std::vector<PartitionEntry> m_partition_entries;
	std::vector<RawDataEntry> m_raw_data_entries;
	std::vector<GroupEntry> m_group_entries;

	std::map<u64, DataEntry> m_data_entries;
};

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Initialize() {
	if (!m_source->ReadAt(0, &m_header_1, sizeof(m_header_1)))
		return false;

	if ((!RVZ && m_header_1.magic != WIA_MAGIC) || (RVZ && m_header_1.magic != RVZ_MAGIC))
		return false;

	const u32 version = RVZ ? RVZ_VERSION : WIA_VERSION;
	const u32 version_read_compatible = RVZ ? RVZ_VERSION_READ_COMPATIBLE : WIA_VERSION_READ_COMPATIBLE;

	const u32 file_version = swap32(m_header_1.version);
	const u32 file_version_compatible = swap32(m_header_1.version_compatible);

	if (version < file_version_compatible || version_read_compatible > file_version)
		return false;  // Unsupported version

	const SHA1::Digest header_1_actual_hash =
	    SHA1::CalculateDigest(&m_header_1, sizeof(m_header_1) - SHA1::DIGEST_LEN);
	if (m_header_1.header_1_hash != header_1_actual_hash)
		return false;

	if (swap64(m_header_1.wia_file_size) != m_source->Size())
		return false;  // File size is incorrect

	const u32 header_2_size = swap32(m_header_1.header_2_size);
	const u32 header_2_min_size = sizeof(WIAHeader2) - sizeof(WIAHeader2::compressor_data);
	if (header_2_size < header_2_min_size || header_2_size > m_source->Size())
		return false;

	std::vector<u8> header_2(header_2_size);
	if (!m_source->ReadAt(sizeof(WIAHeader1), header_2.data(), header_2.size()))
		return false;

	const SHA1::Digest header_2_actual_hash = SHA1::CalculateDigest(header_2.data(), header_2.size());
	if (m_header_1.header_2_hash != header_2_actual_hash)
		return false;

	std::memcpy(&m_header_2, header_2.data(), std::min(header_2.size(), sizeof(WIAHeader2)));

	if (m_header_2.compressor_data_size > sizeof(WIAHeader2::compressor_data) ||
	    header_2_size < header_2_min_size + m_header_2.compressor_data_size) {
		return false;
	}

	const u32 chunk_size = swap32(m_header_2.chunk_size);
	const auto is_power_of_two = [](u32 x) { return (x & (x - 1)) == 0; };
	if ((!RVZ || chunk_size < Wii::BLOCK_TOTAL_SIZE || !is_power_of_two(chunk_size)) &&
	    chunk_size % Wii::GROUP_TOTAL_SIZE != 0) {
		return false;
	}
	if (chunk_size == 0)
		return false;

	const u32 compression_type = swap32(m_header_2.compression_type);
	m_compression_type = static_cast<CompressionType>(compression_type);
	if (m_compression_type > (RVZ ? CompressionType::Zstd : CompressionType::LZMA2) ||
	    (RVZ && m_compression_type == CompressionType::Purge)) {
		return false;  // Unsupported compression type
	}

	const std::size_t number_of_partition_entries = swap32(m_header_2.number_of_partition_entries);
	const std::size_t partition_entry_size = swap32(m_header_2.partition_entry_size);
	if (static_cast<u64>(partition_entry_size) * number_of_partition_entries > m_source->Size())
		return false;
	std::vector<u8> partition_entries(partition_entry_size * number_of_partition_entries);
	if (!m_source->ReadAt(swap64(m_header_2.partition_entries_offset), partition_entries.data(),
	                      partition_entries.size())) {
		return false;
	}

	const SHA1::Digest partition_entries_actual_hash =
	    SHA1::CalculateDigest(partition_entries.data(), partition_entries.size());
	if (m_header_2.partition_entries_hash != partition_entries_actual_hash)
		return false;

	const std::size_t copy_length = std::min(partition_entry_size, sizeof(PartitionEntry));
	const std::size_t memset_length = sizeof(PartitionEntry) - copy_length;
	u8* ptr = partition_entries.data();
	m_partition_entries.resize(number_of_partition_entries);
	for (std::size_t i = 0; i < number_of_partition_entries; ++i, ptr += partition_entry_size) {
		std::memcpy(&m_partition_entries[i], ptr, copy_length);
		std::memset(reinterpret_cast<u8*>(&m_partition_entries[i]) + copy_length, 0, memset_length);
	}

	for (std::size_t i = 0; i < m_partition_entries.size(); ++i) {
		const std::array<PartitionDataEntry, 2>& entries = m_partition_entries[i].data_entries;

		std::size_t non_empty_entries = 0;
		for (std::size_t j = 0; j < entries.size(); ++j) {
			const u32 number_of_sectors = swap32(entries[j].number_of_sectors);
			if (number_of_sectors != 0) {
				++non_empty_entries;
				const u64 last_sector = static_cast<u64>(swap32(entries[j].first_sector)) + number_of_sectors;
				m_data_entries.emplace(last_sector * Wii::BLOCK_TOTAL_SIZE, DataEntry(i, j));
			}
		}

		if (non_empty_entries > 1) {
			if (swap32(entries[0].first_sector) > swap32(entries[1].first_sector))
				return false;
		}
	}

	const u32 number_of_raw_data_entries = swap32(m_header_2.number_of_raw_data_entries);
	if (static_cast<u64>(number_of_raw_data_entries) * sizeof(RawDataEntry) > 0x10000000)
		return false;
	m_raw_data_entries.resize(number_of_raw_data_entries);
	Chunk& raw_data_entries =
	    ReadCompressedData(swap64(m_header_2.raw_data_entries_offset), swap32(m_header_2.raw_data_entries_size),
	                       number_of_raw_data_entries * sizeof(RawDataEntry), m_compression_type);
	if (!raw_data_entries.ReadAll(&m_raw_data_entries))
		return false;

	for (std::size_t i = 0; i < m_raw_data_entries.size(); ++i) {
		const RawDataEntry& entry = m_raw_data_entries[i];
		const u64 data_size = swap64(entry.data_size);
		if (data_size != 0)
			m_data_entries.emplace(swap64(entry.data_offset) + data_size, DataEntry(i));
	}

	const u32 number_of_group_entries = swap32(m_header_2.number_of_group_entries);
	if (static_cast<u64>(number_of_group_entries) * sizeof(GroupEntry) > 0x10000000)
		return false;
	m_group_entries.resize(number_of_group_entries);
	Chunk& group_entries =
	    ReadCompressedData(swap64(m_header_2.group_entries_offset), swap32(m_header_2.group_entries_size),
	                       number_of_group_entries * sizeof(GroupEntry), m_compression_type);
	if (!group_entries.ReadAll(&m_group_entries))
		return false;
	m_cached_chunk_offset = std::numeric_limits<u64>::max();
	m_cached_chunk = Chunk();

	if (HasDataOverlap())
		return false;

	return true;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::HasDataOverlap() const {
	for (std::size_t i = 0; i < m_partition_entries.size(); ++i) {
		const std::array<PartitionDataEntry, 2>& entries = m_partition_entries[i].data_entries;
		for (std::size_t j = 0; j < entries.size(); ++j) {
			if (swap32(entries[j].number_of_sectors) == 0)
				continue;

			const u64 data_offset = swap32(entries[j].first_sector) * Wii::BLOCK_TOTAL_SIZE;
			const auto it = m_data_entries.upper_bound(data_offset);
			if (it == m_data_entries.end())
				return true;  // Not an overlap, but an error nonetheless
			if (!it->second.is_partition || it->second.index != i || it->second.partition_data_index != j)
				return true;  // Overlap
		}
	}

	for (std::size_t i = 0; i < m_raw_data_entries.size(); ++i) {
		if (swap64(m_raw_data_entries[i].data_size) == 0)
			continue;

		const u64 data_offset = swap64(m_raw_data_entries[i].data_offset);
		const auto it = m_data_entries.upper_bound(data_offset);
		if (it == m_data_entries.end())
			return true;  // Not an overlap, but an error nonetheless
		if (it->second.is_partition || it->second.index != i)
			return true;  // Overlap
	}

	return false;
}

template <bool RVZ>
std::vector<u64> WIARVZFileReader<RVZ>::GetSplitPoints() const {
	const u64 iso_size = GetDataSize();
	const u64 chunk_size = GetChunkSize();
	std::vector<u64> points{0, iso_size};

	const auto add_range = [&](u64 start, u64 end, u64 base, u64 step) {
		points.push_back(start);
		points.push_back(end);
		for (u64 p = base + step; p < end; p += step) {
			if (p > start)
				points.push_back(p);
		}
	};

	for (const RawDataEntry& entry : m_raw_data_entries) {
		const u64 offset = swap64(entry.data_offset);
		const u64 size = swap64(entry.data_size);
		if (size != 0)
			add_range(offset, offset + size, offset - offset % Wii::BLOCK_TOTAL_SIZE, chunk_size);
	}
	for (const PartitionEntry& partition : m_partition_entries) {
		for (const PartitionDataEntry& data : partition.data_entries) {
			const u64 offset = swap32(data.first_sector) * Wii::BLOCK_TOTAL_SIZE;
			const u64 size = swap32(data.number_of_sectors) * Wii::BLOCK_TOTAL_SIZE;
			if (size != 0)
				add_range(offset, offset + size, offset, std::max<u64>(chunk_size, Wii::GROUP_TOTAL_SIZE));
		}
	}

	std::sort(points.begin(), points.end());
	points.erase(std::unique(points.begin(), points.end()), points.end());
	while (!points.empty() && points.back() > iso_size)
		points.pop_back();
	return points;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Read(u64 offset, u64 size, u8* out_ptr) {
	if (offset + size > swap64(m_header_1.iso_file_size) || offset + size < offset)
		return false;

	if (offset < sizeof(WIAHeader2::disc_header)) {
		const u64 bytes_to_read = std::min<u64>(sizeof(WIAHeader2::disc_header) - offset, size);
		std::memcpy(out_ptr, m_header_2.disc_header.data() + offset, bytes_to_read);
		offset += bytes_to_read;
		size -= bytes_to_read;
		out_ptr += bytes_to_read;
	}

	const u32 chunk_size = swap32(m_header_2.chunk_size);
	while (size > 0) {
		const auto it = m_data_entries.upper_bound(offset);
		if (it == m_data_entries.end())
			return false;

		const DataEntry& data = it->second;
		if (data.is_partition) {
			const PartitionEntry& partition = m_partition_entries[it->second.index];

			const u32 partition_first_sector = swap32(partition.data_entries[0].first_sector);
			const u64 partition_data_offset = partition_first_sector * Wii::BLOCK_TOTAL_SIZE;

			const u32 second_number_of_sectors = swap32(partition.data_entries[1].number_of_sectors);
			const u32 partition_total_sectors =
			    second_number_of_sectors ?
			        swap32(partition.data_entries[1].first_sector) - partition_first_sector + second_number_of_sectors :
			        swap32(partition.data_entries[0].number_of_sectors);

			for (const PartitionDataEntry& partition_data : partition.data_entries) {
				if (size == 0)
					return true;

				const u32 first_sector = swap32(partition_data.first_sector);
				const u32 number_of_sectors = swap32(partition_data.number_of_sectors);

				const u64 data_offset = first_sector * Wii::BLOCK_TOTAL_SIZE;
				const u64 data_size = number_of_sectors * Wii::BLOCK_TOTAL_SIZE;

				if (data_size == 0)
					continue;
				if (data_offset + data_size <= offset)
					continue;
				if (offset < data_offset)
					return false;

				const u64 bytes_to_read = std::min(data_size - (offset - data_offset), size);

				m_exception_list.clear();
				m_write_to_exception_list = true;
				m_exception_list_last_group_index = std::numeric_limits<u64>::max();

				bool hash_exception_error = false;
				const bool ok = EncryptGroups(offset - partition_data_offset, bytes_to_read, out_ptr,
				                              partition_data_offset, partition_total_sectors * Wii::BLOCK_DATA_SIZE,
				                              partition.partition_key, &hash_exception_error);
				m_write_to_exception_list = false;
				if (!ok || hash_exception_error)
					return false;

				offset += bytes_to_read;
				size -= bytes_to_read;
				out_ptr += bytes_to_read;
			}
		} else {
			const RawDataEntry& raw_data = m_raw_data_entries[data.index];
			if (!ReadFromGroups(&offset, &size, &out_ptr, chunk_size, Wii::BLOCK_TOTAL_SIZE,
			                    swap64(raw_data.data_offset), swap64(raw_data.data_size), swap32(raw_data.group_index),
			                    swap32(raw_data.number_of_groups), 0)) {
				return false;
			}
		}
	}

	return true;
}

template <bool RVZ>
const std::array<u8, Wii::GROUP_TOTAL_SIZE>* WIARVZFileReader<RVZ>::EncryptGroup(u64 offset, u64 partition_data_offset,
                                                                                u64 partition_data_decrypted_size,
                                                                                const WiiKey& key,
                                                                                bool* hash_exception_error) {
	// Only allocate memory if this function actually ends up getting called
	if (!m_encryption_cache) {
		m_encryption_cache = std::make_unique<std::array<u8, Wii::GROUP_TOTAL_SIZE>>();
		m_encryption_cache_offset = std::numeric_limits<u64>::max();
	}

	const u64 group_offset_in_partition = offset / Wii::GROUP_TOTAL_SIZE * Wii::GROUP_DATA_SIZE;
	const u64 group_offset_on_disc = partition_data_offset + offset;

	if (m_encryption_cache_offset == group_offset_on_disc)
		return m_encryption_cache.get();
	m_encryption_cache_offset = std::numeric_limits<u64>::max();

	std::vector<Wii::BlockData> unencrypted_data(Wii::BLOCKS_PER_GROUP);
	std::vector<Wii::HashBlock> unencrypted_hashes(Wii::BLOCKS_PER_GROUP);

	for (std::size_t block = 0; block < Wii::BLOCKS_PER_GROUP; ++block) {
		const u64 block_offset = group_offset_in_partition + block * Wii::BLOCK_DATA_SIZE;
		if (block_offset + Wii::BLOCK_DATA_SIZE <= partition_data_decrypted_size) {
			if (!ReadWiiDecrypted(block_offset, Wii::BLOCK_DATA_SIZE, unencrypted_data[block].data(),
			                      partition_data_offset)) {
				return nullptr;
			}
		} else {
			unencrypted_data[block].fill(0);
		}
	}

	Wii::HashGroup(unencrypted_data.data(), unencrypted_hashes.data());

	// ReadWiiDecrypted calls ReadFromGroups, which populated m_exception_list
	if (m_write_to_exception_list && !ApplyHashExceptions(m_exception_list, unencrypted_hashes.data()))
		*hash_exception_error = true;

	const AES128 aes(key.data(), AES128::Mode::Encrypt);
	for (std::size_t j = 0; j < Wii::BLOCKS_PER_GROUP; ++j) {
		Wii::EncryptBlock(unencrypted_hashes[j], unencrypted_data[j].data(),
		                  m_encryption_cache->data() + j * Wii::BLOCK_TOTAL_SIZE, aes);
	}

	m_encryption_cache_offset = group_offset_on_disc;
	return m_encryption_cache.get();
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::EncryptGroups(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset,
                                          u64 partition_data_decrypted_size, const WiiKey& key,
                                          bool* hash_exception_error) {
	while (size > 0) {
		const u64 group_offset = AlignDown(offset, Wii::GROUP_TOTAL_SIZE);
		if (m_encryption_cache_offset != partition_data_offset + group_offset) {
			// A new group: start a new exception list for it
			m_exception_list.clear();
			m_exception_list_last_group_index = std::numeric_limits<u64>::max();
		}
		const std::array<u8, Wii::GROUP_TOTAL_SIZE>* group =
		    EncryptGroup(group_offset, partition_data_offset, partition_data_decrypted_size, key, hash_exception_error);
		if (!group)
			return false;

		const u64 offset_in_group = offset % Wii::GROUP_TOTAL_SIZE;
		const u64 bytes_to_read = std::min(Wii::GROUP_TOTAL_SIZE - offset_in_group, size);
		std::memcpy(out_ptr, group->data() + offset_in_group, bytes_to_read);

		offset += bytes_to_read;
		size -= bytes_to_read;
		out_ptr += bytes_to_read;
	}
	return true;
}

template <bool RVZ>
const PartitionEntry* WIARVZFileReader<RVZ>::GetPartition(u64 partition_data_offset, u32* partition_first_sector) const {
	const auto it = m_data_entries.upper_bound(partition_data_offset);
	if (it == m_data_entries.end() || !it->second.is_partition)
		return nullptr;

	const PartitionEntry* partition = &m_partition_entries[it->second.index];
	*partition_first_sector = swap32(partition->data_entries[0].first_sector);
	if (partition_data_offset != *partition_first_sector * Wii::BLOCK_TOTAL_SIZE)
		return nullptr;

	return partition;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const {
	u32 partition_first_sector;
	const PartitionEntry* partition = GetPartition(partition_data_offset, &partition_first_sector);
	if (!partition)
		return false;

	for (const PartitionDataEntry& data : partition->data_entries) {
		const u32 start_sector = swap32(data.first_sector) - partition_first_sector;
		const u32 end_sector = start_sector + swap32(data.number_of_sectors);
		if (offset + size <= end_sector * Wii::BLOCK_DATA_SIZE)
			return true;
	}

	return false;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) {
	u32 partition_first_sector;
	const PartitionEntry* partition = GetPartition(partition_data_offset, &partition_first_sector);
	if (!partition)
		return false;

	const u64 chunk_size = swap32(m_header_2.chunk_size) * Wii::BLOCK_DATA_SIZE / Wii::BLOCK_TOTAL_SIZE;

	for (const PartitionDataEntry& data : partition->data_entries) {
		if (size == 0)
			return true;

		const u64 data_offset = (swap32(data.first_sector) - partition_first_sector) * Wii::BLOCK_DATA_SIZE;
		const u64 data_size = swap32(data.number_of_sectors) * Wii::BLOCK_DATA_SIZE;

		if (!ReadFromGroups(&offset, &size, &out_ptr, chunk_size, Wii::BLOCK_DATA_SIZE, data_offset, data_size,
		                    swap32(data.group_index), swap32(data.number_of_groups),
		                    std::max<u32>(1, static_cast<u32>(chunk_size / Wii::GROUP_DATA_SIZE)))) {
			return false;
		}
	}

	return size == 0;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ReadFromGroups(u64* offset, u64* size, u8** out_ptr, u64 chunk_size, u32 sector_size,
                                           u64 data_offset, u64 data_size, u32 group_index, u32 number_of_groups,
                                           u32 exception_lists) {
	if (data_offset + data_size <= *offset)
		return true;

	if (*offset < data_offset)
		return false;

	const u64 skipped_data = data_offset % sector_size;
	data_offset -= skipped_data;
	data_size += skipped_data;

	const u64 start_group_index = (*offset - data_offset) / chunk_size;
	for (u64 i = start_group_index; i < number_of_groups && (*size) > 0; ++i) {
		const u64 total_group_index = group_index + i;
		if (total_group_index >= m_group_entries.size())
			return false;

		const GroupEntry group = m_group_entries[total_group_index];
		const u64 group_offset_in_data = i * chunk_size;
		const u64 offset_in_group = *offset - group_offset_in_data - data_offset;

		chunk_size = std::min(chunk_size, data_size - group_offset_in_data);

		const u64 bytes_to_read = std::min(chunk_size - offset_in_group, *size);
		u32 group_data_size = swap32(group.data_size);

		CompressionType compression_type = m_compression_type;
		u32 rvz_packed_size = 0;
		if constexpr (RVZ) {
			if ((group_data_size & 0x80000000) == 0)
				compression_type = CompressionType::None;

			group_data_size &= 0x7FFFFFFF;

			rvz_packed_size = swap32(group.rvz_packed_size);
		}

		if (group_data_size == 0) {
			std::memset(*out_ptr, 0, bytes_to_read);
		} else {
			const u64 group_offset_in_file = static_cast<u64>(swap32(group.data_offset)) << 2;

			Chunk& chunk = ReadCompressedData(group_offset_in_file, group_data_size, chunk_size, compression_type,
			                                  exception_lists, rvz_packed_size, group_offset_in_data);

			if (!chunk.Read(offset_in_group, bytes_to_read, *out_ptr)) {
				m_cached_chunk_offset = std::numeric_limits<u64>::max();  // Invalidate the cache
				return false;
			}

			if (m_write_to_exception_list && m_exception_list_last_group_index != total_group_index) {
				const u64 exception_list_index = offset_in_group / Wii::GROUP_DATA_SIZE;
				const u16 additional_offset = static_cast<u16>(group_offset_in_data % Wii::GROUP_DATA_SIZE /
				                                               Wii::BLOCK_DATA_SIZE * Wii::BLOCK_HEADER_SIZE);
				chunk.GetHashExceptions(&m_exception_list, exception_list_index, additional_offset);
				m_exception_list_last_group_index = total_group_index;
			}
		}

		*offset += bytes_to_read;
		*size -= bytes_to_read;
		*out_ptr += bytes_to_read;
	}

	return true;
}

template <bool RVZ>
typename WIARVZFileReader<RVZ>::Chunk& WIARVZFileReader<RVZ>::ReadCompressedData(u64 offset_in_file, u64 compressed_size,
                                                                                u64 decompressed_size,
                                                                                CompressionType compression_type,
                                                                                u32 exception_lists, u32 rvz_packed_size,
                                                                                u64 data_offset) {
	if (offset_in_file == m_cached_chunk_offset)
		return m_cached_chunk;

	const bool compressed_exception_lists = compression_type > CompressionType::Purge;

	// Generous bound on what the stream decompresses to, used to size LZMA dictionaries
	const u64 max_output = decompressed_size + rvz_packed_size + 0x10000 * (static_cast<u64>(exception_lists) + 1);

	std::unique_ptr<Decompressor> decompressor =
	    CreateDecompressor(compression_type, m_header_2.compressor_data, m_header_2.compressor_data_size, max_output,
	                       rvz_packed_size == 0 ? decompressed_size : rvz_packed_size);

	m_cached_chunk = Chunk(m_source.get(), offset_in_file, compressed_size, decompressed_size, exception_lists,
	                       compressed_exception_lists, rvz_packed_size, data_offset, std::move(decompressor));
	m_cached_chunk_offset = offset_in_file;
	return m_cached_chunk;
}

template <bool RVZ>
WIARVZFileReader<RVZ>::Chunk::Chunk(Source* source, u64 offset_in_file, u64 compressed_size, u64 decompressed_size,
                                    u32 exception_lists, bool compressed_exception_lists, u32 rvz_packed_size,
                                    u64 data_offset, std::unique_ptr<Decompressor> decompressor)
    : m_decompressor(std::move(decompressor)), m_source(source), m_offset_in_file(offset_in_file),
      m_exception_lists(exception_lists), m_compressed_exception_lists(compressed_exception_lists),
      m_rvz_packed_size(rvz_packed_size), m_data_offset(data_offset) {
	constexpr std::size_t MAX_SIZE_PER_EXCEPTION_LIST =
	    AlignUp(Wii::BLOCK_HEADER_SIZE, SHA1::DIGEST_LEN) / SHA1::DIGEST_LEN * Wii::BLOCKS_PER_GROUP *
	        sizeof(HashExceptionEntry) +
	    sizeof(u16);

	m_out_bytes_allocated_for_exceptions =
	    m_compressed_exception_lists ? MAX_SIZE_PER_EXCEPTION_LIST * m_exception_lists : 0;

	// A corrupted file could claim anything; don't try to allocate absurd amounts
	if (compressed_size > 0x40000000 || decompressed_size > 0x40000000) {
		m_decompressor.reset();
		return;
	}

	m_in.data.resize(compressed_size);
	m_out.data.resize(decompressed_size + m_out_bytes_allocated_for_exceptions);
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Chunk::Read(u64 offset, u64 size, u8* out_ptr) {
	if (!m_decompressor || !m_source || offset + size > m_out.data.size() - m_out_bytes_allocated_for_exceptions)
		return false;

	while (offset + size > GetOutBytesWrittenExcludingExceptions()) {
		u64 bytes_to_read;
		if (offset + size == m_out.data.size()) {
			// Read all the remaining data.
			bytes_to_read = m_in.data.size() - m_in.bytes_written;
		} else {
			// Pick a suitable amount of compressed data to read. It has to be larger than 0 and no
			// larger than what is available to read, but the rest is a bit arbitrary.

			// The compressed data is probably not much bigger than the decompressed data.
			// Add a few bytes for possible compression overhead and for any hash exceptions.
			bytes_to_read = offset + size - GetOutBytesWrittenExcludingExceptions() + 0x100;

			// Align the access in an attempt to gain speed. We don't know the block size of the
			// underlying storage device, so we just use the Wii block size.
			bytes_to_read = AlignUp(bytes_to_read + m_offset_in_file, Wii::BLOCK_TOTAL_SIZE) - m_offset_in_file;

			// Ensure we don't read too much.
			bytes_to_read = std::min<u64>(m_in.data.size() - m_in.bytes_written, bytes_to_read);
		}

		if (bytes_to_read == 0) {
			// Compressed size is larger than expected or decompressed size is smaller than expected
			return false;
		}

		if (!m_source->ReadAt(m_offset_in_file, m_in.data.data() + m_in.bytes_written,
		                      static_cast<std::size_t>(bytes_to_read))) {
			return false;
		}

		m_offset_in_file += bytes_to_read;
		m_in.bytes_written += bytes_to_read;

		if (m_exception_lists > 0 && !m_compressed_exception_lists) {
			if (!HandleExceptions(m_in.data.data(), m_in.data.size(), m_in.bytes_written,
			                      &m_in_bytes_used_for_exceptions, true)) {
				return false;
			}

			m_in_bytes_read = m_in_bytes_used_for_exceptions;
		}

		if (m_exception_lists == 0 || m_compressed_exception_lists) {
			if (!Decompress())
				return false;
		}

		if (m_exception_lists > 0 && m_compressed_exception_lists) {
			if (!HandleExceptions(m_out.data.data(), m_out_bytes_allocated_for_exceptions, m_out.bytes_written,
			                      &m_out_bytes_used_for_exceptions, false)) {
				return false;
			}

			if (m_rvz_packed_size != 0 && m_exception_lists == 0) {
				if (!Decompress())
					return false;
			}
		}

		if (m_exception_lists == 0) {
			const std::size_t expected_out_bytes =
			    m_out.data.size() - m_out_bytes_allocated_for_exceptions + m_out_bytes_used_for_exceptions;

			if (m_out.bytes_written > expected_out_bytes)
				return false;  // Decompressed size is larger than expected

			// The m_in.bytes_written == m_in.data.size() check is needed because (for example) zstd
			// can finish writing all data to m_out before becoming done if it has been given all
			// input data except the checksum at the end.
			if (m_out.bytes_written == expected_out_bytes && !m_decompressor->Done() &&
			    m_in.bytes_written == m_in.data.size()) {
				return false;  // Decompressed size is larger than expected
			}

			if (m_decompressor->Done() && m_in_bytes_read != m_in.data.size())
				return false;  // Compressed size is smaller than expected
		}
	}

	std::memcpy(out_ptr, m_out.data.data() + offset + m_out_bytes_used_for_exceptions, size);
	return true;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Chunk::Decompress() {
	if (m_rvz_packed_size != 0 && m_exception_lists == 0) {
		const std::size_t bytes_to_move = m_out.bytes_written - m_out_bytes_used_for_exceptions;

		DecompressionBuffer in{std::vector<u8>(bytes_to_move), bytes_to_move};
		if (bytes_to_move > 0)
			std::memcpy(in.data.data(), m_out.data.data() + m_out_bytes_used_for_exceptions, bytes_to_move);

		m_out.bytes_written = m_out_bytes_used_for_exceptions;

		m_decompressor =
		    CreateRVZPackDecompressor(std::move(m_decompressor), std::move(in), m_data_offset, m_rvz_packed_size);

		m_rvz_packed_size = 0;
	}

	return m_decompressor->Decompress(m_in, &m_out, &m_in_bytes_read);
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Chunk::HandleExceptions(const u8* data, std::size_t bytes_allocated,
                                                    std::size_t bytes_written, std::size_t* bytes_used, bool align) {
	while (m_exception_lists > 0) {
		if (sizeof(u16) + *bytes_used > bytes_allocated)
			return false;  // More hash exceptions than expected
		if (sizeof(u16) + *bytes_used > bytes_written)
			return true;

		const u16 exceptions = swap16(data + *bytes_used);

		std::size_t exception_list_size = exceptions * sizeof(HashExceptionEntry) + sizeof(u16);
		if (align && m_exception_lists == 1)
			exception_list_size = AlignUp(*bytes_used + exception_list_size, 4) - *bytes_used;

		if (exception_list_size + *bytes_used > bytes_allocated)
			return false;  // More hash exceptions than expected
		if (exception_list_size + *bytes_used > bytes_written)
			return true;

		*bytes_used += exception_list_size;
		--m_exception_lists;
	}

	return true;
}

template <bool RVZ>
void WIARVZFileReader<RVZ>::Chunk::GetHashExceptions(std::vector<HashExceptionEntry>* exception_list,
                                                     u64 exception_list_index, u16 additional_offset) const {
	const u8* data_start = m_compressed_exception_lists ? m_out.data.data() : m_in.data.data();
	const u8* data_end =
	    data_start + (m_compressed_exception_lists ? m_out_bytes_used_for_exceptions : m_in_bytes_used_for_exceptions);
	const u8* data = data_start;

	for (u64 i = exception_list_index; i > 0; --i) {
		if (data + sizeof(u16) > data_end)
			return;
		data += swap16(data) * sizeof(HashExceptionEntry) + sizeof(u16);
	}
	if (data + sizeof(u16) > data_end)
		return;

	const u16 exceptions = swap16(data);
	data += sizeof(u16);

	for (std::size_t i = 0; i < exceptions && data + sizeof(HashExceptionEntry) <= data_end; ++i) {
		std::memcpy(&exception_list->emplace_back(), data, sizeof(HashExceptionEntry));
		data += sizeof(HashExceptionEntry);

		u16& offset = exception_list->back().offset;
		offset = swap16(static_cast<u16>(swap16(offset) + additional_offset));
	}
}

template <bool RVZ>
std::size_t WIARVZFileReader<RVZ>::Chunk::GetOutBytesWrittenExcludingExceptions() const {
	return m_exception_lists == 0 ? m_out.bytes_written - m_out_bytes_used_for_exceptions : 0;
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ApplyHashExceptions(const std::vector<HashExceptionEntry>& exception_list,
                                                Wii::HashBlock hash_blocks[Wii::BLOCKS_PER_GROUP]) {
	for (const HashExceptionEntry& exception : exception_list) {
		const u16 offset = swap16(exception.offset);

		const std::size_t block_index = offset / Wii::BLOCK_HEADER_SIZE;
		if (block_index >= Wii::BLOCKS_PER_GROUP)
			return false;

		const std::size_t offset_in_block = offset % Wii::BLOCK_HEADER_SIZE;
		if (offset_in_block + SHA1::DIGEST_LEN > Wii::BLOCK_HEADER_SIZE)
			return false;

		std::memcpy(reinterpret_cast<u8*>(&hash_blocks[block_index]) + offset_in_block, exception.hash.data(),
		            SHA1::DIGEST_LEN);
	}

	return true;
}

// ---- Writer -----------------------------------------------------------------------------------

struct ReuseID {
	auto Tie() const { return std::tie(partition_key, data_size, encrypted, value); }
	bool operator==(const ReuseID& other) const { return Tie() == other.Tie(); }
	bool operator<(const ReuseID& other) const { return Tie() < other.Tie(); }

	WiiKey partition_key;
	u64 data_size;
	bool encrypted;
	u8 value;
};

struct CompressThreadState {
	std::unique_ptr<Compressor> compressor;
	std::vector<Wii::BlockData> decryption_buffer = std::vector<Wii::BlockData>(Wii::BLOCKS_PER_GROUP);
	std::vector<Wii::HashBlock> hash_buffer = std::vector<Wii::HashBlock>(Wii::BLOCKS_PER_GROUP);
};

struct CompressParameters {
	std::vector<u8> data{};
	const DataEntry* data_entry = nullptr;
	u64 data_offset = 0;
	u64 bytes_read = 0;
	std::size_t group_index = 0;
};

template <bool RVZ>
struct Writer {
	using GroupEntry = std::conditional_t<RVZ, RVZGroupEntry, WIAGroupEntry>;

	struct OutputParametersEntry {
		std::vector<u8> exception_lists;
		std::vector<u8> main_data;
		std::optional<ReuseID> reuse_id;
		std::optional<GroupEntry> reused_group;
		std::size_t rvz_packed_size = 0;  // RVZ only
		bool compressed = false;          // RVZ only
	};

	struct OutputParameters {
		std::vector<OutputParametersEntry> entries;
		u64 bytes_read = 0;
		std::size_t group_index = 0;
	};

	// Tracks the write position like a file would, so Dolphin's sequential writing logic carries over.
	struct Output {
		Sink* sink;
		u64 position = 0;
		u64 size = 0;
		bool Write(const void* data, std::size_t len) {
			if (len && !sink->WriteAt(position, data, len))
				return false;
			position += len;
			size = std::max(size, position);
			return true;
		}
		bool Write(const std::vector<u8>& data) { return Write(data.data(), data.size()); }
	};

	static bool PadTo4(Output* file, u64* bytes_written) {
		constexpr u32 ZEROES = 0;
		const u64 bytes_to_write = AlignUp(*bytes_written, 4) - *bytes_written;
		if (bytes_to_write == 0)
			return true;
		*bytes_written += bytes_to_write;
		return file->Write(&ZEROES, static_cast<std::size_t>(bytes_to_write));
	}

	static void AddRawDataEntry(u64 offset, u64 size, int chunk_size, u32* total_groups,
	                            std::vector<RawDataEntry>* raw_data_entries, std::vector<DataEntry>* data_entries) {
		constexpr std::size_t SKIP_SIZE = sizeof(WIAHeader2::disc_header);
		const u64 skip = offset < SKIP_SIZE ? std::min<u64>(SKIP_SIZE - offset, size) : 0;

		offset += skip;
		size -= skip;

		if (size == 0)
			return;

		const u32 group_index = *total_groups;
		const u32 groups = static_cast<u32>(AlignUp(size, chunk_size) / chunk_size);
		*total_groups += groups;

		data_entries->emplace_back(raw_data_entries->size());
		raw_data_entries->emplace_back(
		    RawDataEntry{swap64(offset), swap64(size), swap32(group_index), swap32(groups)});
	}

	static PartitionDataEntry CreatePartitionDataEntry(u64 offset, u64 size, u32 index, int chunk_size,
	                                                   u32* total_groups,
	                                                   const std::vector<PartitionEntry>& partition_entries,
	                                                   std::vector<DataEntry>* data_entries) {
		const u32 group_index = *total_groups;
		const u64 rounded_size = AlignDown(size, Wii::BLOCK_TOTAL_SIZE);
		const u32 groups = static_cast<u32>(AlignUp(rounded_size, chunk_size) / chunk_size);
		*total_groups += groups;

		data_entries->emplace_back(partition_entries.size(), index);
		return PartitionDataEntry{swap32(static_cast<u32>(offset / Wii::BLOCK_TOTAL_SIZE)),
		                          swap32(static_cast<u32>(size / Wii::BLOCK_TOTAL_SIZE)), swap32(group_index),
		                          swap32(groups)};
	}

	static ConversionResultCode SetUpDataEntriesForWriting(const Volume* volume, int chunk_size, u64 iso_size,
	                                                       u32* total_groups,
	                                                       std::vector<PartitionEntry>* partition_entries,
	                                                       std::vector<RawDataEntry>* raw_data_entries,
	                                                       std::vector<DataEntry>* data_entries,
	                                                       std::vector<std::unique_ptr<FileSystem>>* partition_file_systems) {
		std::vector<Volume::Partition> partitions;
		if (volume && volume->HasWiiHashes() && volume->HasWiiEncryption())
			partitions = volume->GetPartitions();  // sorted by offset

		*total_groups = 0;

		u64 last_partition_end_offset = 0;

		const auto add_raw_data_entry = [&](u64 offset, u64 size) {
			return AddRawDataEntry(offset, size, chunk_size, total_groups, raw_data_entries, data_entries);
		};

		const auto create_partition_data_entry = [&](u64 offset, u64 size, u32 index) {
			return CreatePartitionDataEntry(offset, size, index, chunk_size, total_groups, *partition_entries,
			                                data_entries);
		};

		for (const Volume::Partition& partition : partitions) {
			// If a partition is odd in some way that prevents encoding it as a partition, it is
			// encoded as raw data instead by skipping the current loop iteration. Partitions can
			// always be encoded as raw data, but it is less space efficient.

			if (partition.offset < last_partition_end_offset)
				continue;  // Overlapping partitions

			if (volume->ReadSwapped32(partition.offset, nullptr) != 0x10001U) {
				// This looks more like garbage data than an actual partition. Some WBFS writing
				// programs scrub the SSBB Masterpiece partitions without removing them from the
				// partition table, causing this.
				continue;
			}

			std::optional<u64> data_offset = volume->ReadSwappedAndShifted(partition.offset + 0x2b8, nullptr);
			std::optional<u64> data_size = volume->ReadSwappedAndShifted(partition.offset + 0x2bc, nullptr);

			if (!data_offset || !data_size)
				return ConversionResultCode::ReadFailed;

			const u64 data_start = partition.offset + *data_offset;
			const u64 data_end = data_start + *data_size;

			if (data_start % Wii::BLOCK_TOTAL_SIZE != 0)
				continue;  // Misaligned partition

			if (*data_size < Wii::BLOCK_TOTAL_SIZE)
				continue;  // Very small partition

			if (data_end > iso_size) {
				// Too large partition
				*data_size = iso_size - *data_offset - partition.offset;
			}

			const std::optional<u64> fst_offset = volume->ReadSwappedAndShifted(0x424, &partition);
			const std::optional<u64> fst_size = volume->ReadSwappedAndShifted(0x428, &partition);

			if (!fst_offset || !fst_size)
				return ConversionResultCode::ReadFailed;

			if (!partition.ticket_valid)
				return ConversionResultCode::ReadFailed;

			add_raw_data_entry(last_partition_end_offset, partition.offset - last_partition_end_offset);

			add_raw_data_entry(partition.offset, *data_offset);

			const u64 fst_end = volume->PartitionOffsetToRawOffset(*fst_offset + *fst_size, partition);
			const u64 split_point =
			    std::min(data_end, AlignUp(fst_end - data_start, Wii::GROUP_TOTAL_SIZE) + data_start);

			PartitionEntry partition_entry;
			partition_entry.partition_key = partition.title_key;
			partition_entry.data_entries[0] = create_partition_data_entry(data_start, split_point - data_start, 0);
			partition_entry.data_entries[1] = create_partition_data_entry(split_point, data_end - split_point, 1);

			// last_partition_end_offset can't simply be data_end, because
			// create_partition_data_entry may have rounded it
			last_partition_end_offset = (static_cast<u64>(swap32(partition_entry.data_entries[1].first_sector)) +
			                             swap32(partition_entry.data_entries[1].number_of_sectors)) *
			                            Wii::BLOCK_TOTAL_SIZE;

			partition_entries->emplace_back(std::move(partition_entry));
			partition_file_systems->emplace_back(volume->CreateFileSystem(&partition));
		}

		add_raw_data_entry(last_partition_end_offset, iso_size - last_partition_end_offset);

		return ConversionResultCode::Success;
	}

	static std::optional<std::vector<u8>> Compress(Compressor* compressor, const u8* data, std::size_t size) {
		if (compressor) {
			if (!compressor->Start(size) || !compressor->Compress(data, size) || !compressor->End())
				return std::nullopt;

			data = compressor->GetData();
			size = compressor->GetSize();
		}

		return std::vector<u8>(data, data + size);
	}

	static bool TryReuse(std::map<ReuseID, GroupEntry>* reusable_groups, std::mutex* reusable_groups_mutex,
	                     OutputParametersEntry* entry) {
		if (entry->reused_group)
			return true;

		if (!entry->reuse_id)
			return false;

		std::lock_guard<std::mutex> guard(*reusable_groups_mutex);
		const auto it = reusable_groups->find(*entry->reuse_id);
		if (it == reusable_groups->end())
			return false;

		entry->reused_group = it->second;
		return true;
	}

	static void RVZPack(const u8* in, OutputParametersEntry* out, u64 bytes_per_chunk, std::size_t chunks,
	                    u64 total_size, u64 data_offset, bool multipart, bool allow_junk_reuse, bool compression,
	                    const FileSystem* file_system) {
		using Seed = std::array<u32, LaggedFibonacciGenerator::SEED_SIZE>;
		struct JunkInfo {
			std::size_t start_offset;
			Seed seed;
		};

		constexpr std::size_t SEED_SIZE = LaggedFibonacciGenerator::SEED_SIZE * sizeof(u32);

		// Maps end_offset -> (start_offset, seed)
		std::map<std::size_t, JunkInfo> junk_info;

		std::size_t position = 0;
		while (position < total_size) {
			// Skip the 0 to 32 zero bytes that typically come after a file
			std::size_t zeroes = 0;
			while (position + zeroes < total_size && in[position + zeroes] == 0)
				++zeroes;

			// If there are very many zero bytes (perhaps the PRNG junk data has been scrubbed?) and
			// we aren't using compression, it makes sense to encode the zero bytes as junk. If we are
			// using compression, the compressor will likely encode zeroes better than we can.
			if (!compression && zeroes > SEED_SIZE)
				junk_info.emplace(position + zeroes, JunkInfo{position, {}});

			position += zeroes;
			data_offset += zeroes;

			const std::size_t bytes_to_read = static_cast<std::size_t>(
			    std::min<u64>(AlignUp(data_offset + 1, Wii::BLOCK_TOTAL_SIZE) - data_offset, total_size - position));

			const std::size_t data_offset_mod = static_cast<std::size_t>(data_offset % Wii::BLOCK_TOTAL_SIZE);

			Seed seed;
			const std::size_t bytes_reconstructed =
			    LaggedFibonacciGenerator::GetSeed(in + position, bytes_to_read, data_offset_mod, seed.data());

			if (bytes_reconstructed > 0)
				junk_info.emplace(position + bytes_reconstructed, JunkInfo{position, seed});

			if (file_system) {
				const std::optional<FileSystem::File> file_info =
				    file_system->FindFileInfo(data_offset + bytes_reconstructed);

				// If we're at a file and there's more space in this block after the file, continue
				// after the file instead of skipping to the next block
				if (file_info) {
					const u64 file_end_offset = file_info->offset + file_info->size;
					if (file_end_offset < data_offset + bytes_to_read) {
						position += static_cast<std::size_t>(file_end_offset - data_offset);
						data_offset = file_end_offset;
						continue;
					}
				}
			}

			position += bytes_to_read;
			data_offset += bytes_to_read;
		}

		for (std::size_t i = 0; i < chunks; ++i) {
			OutputParametersEntry& entry = out[i];
			if (entry.reused_group)
				continue;

			u64 current_offset = i * bytes_per_chunk;
			const u64 end_offset = std::min(current_offset + bytes_per_chunk, total_size);

			const bool store_junk_efficiently = allow_junk_reuse || !entry.reuse_id;

			// It would be possible to support skipping RVZ packing even when the chunk size is larger
			// than 2 MiB (multipart == true), but it would be more effort than it's worth.
			bool first_loop_iteration = !multipart;

			while (current_offset < end_offset) {
				u64 next_junk_start = end_offset;
				u64 next_junk_end = end_offset;
				Seed* seed = nullptr;
				if (store_junk_efficiently && end_offset - current_offset > SEED_SIZE) {
					const auto next_junk_it = junk_info.upper_bound(static_cast<std::size_t>(current_offset + SEED_SIZE));
					if (next_junk_it != junk_info.end() && next_junk_it->second.start_offset + SEED_SIZE < end_offset) {
						next_junk_start = std::max<u64>(current_offset, next_junk_it->second.start_offset);
						next_junk_end = std::min<u64>(end_offset, next_junk_it->first);
						seed = &next_junk_it->second.seed;
					}
				}

				if (first_loop_iteration) {
					if (next_junk_start == end_offset) {
						// Storing this chunk with RVZ packing would be inefficient, so store it without
						PushBack(&entry.main_data, in + current_offset, in + end_offset);
						break;
					}

					first_loop_iteration = false;
				}

				const u64 non_junk_bytes = next_junk_start - current_offset;
				if (non_junk_bytes > 0) {
					const u8* ptr = in + current_offset;

					PushBack(&entry.main_data, swap32(static_cast<u32>(non_junk_bytes)));
					PushBack(&entry.main_data, ptr, ptr + non_junk_bytes);

					current_offset += non_junk_bytes;
					entry.rvz_packed_size += sizeof(u32) + non_junk_bytes;
				}

				const u64 junk_bytes = next_junk_end - current_offset;
				if (junk_bytes > 0) {
					PushBack(&entry.main_data, swap32(static_cast<u32>(junk_bytes) | 0x80000000));
					PushBack(&entry.main_data, *seed);

					current_offset += junk_bytes;
					entry.rvz_packed_size += sizeof(u32) + SEED_SIZE;
				}
			}
		}
	}

	static ConversionResult<OutputParameters> ProcessAndCompress(
	    CompressThreadState* state, CompressParameters parameters, const std::vector<PartitionEntry>& partition_entries,
	    const FileSystem* file_system, std::map<ReuseID, GroupEntry>* reusable_groups,
	    std::mutex* reusable_groups_mutex, u64 chunks_per_wii_group, u64 exception_lists_per_chunk,
	    bool compressed_exception_lists, bool compression) {
		std::vector<OutputParametersEntry> output_entries;

		if (!parameters.data_entry->is_partition) {
			OutputParametersEntry& entry = output_entries.emplace_back();
			std::vector<u8>& data = parameters.data;

			if (AllSame(data))
				entry.reuse_id = ReuseID{WiiKey{}, data.size(), false, data.front()};

			if constexpr (RVZ) {
				RVZPack(data.data(), output_entries.data(), data.size(), 1, data.size(), parameters.data_offset, false,
				        true, compression, file_system);
			} else {
				entry.main_data = std::move(data);
			}
		} else {
			const PartitionEntry& partition_entry = partition_entries[parameters.data_entry->index];

			const AES128 aes(partition_entry.partition_key.data(), AES128::Mode::Decrypt);

			const u64 groups = AlignUp(parameters.data.size(), Wii::GROUP_TOTAL_SIZE) / Wii::GROUP_TOTAL_SIZE;

			const u64 blocks = parameters.data.size() / Wii::BLOCK_TOTAL_SIZE;

			const u64 blocks_per_chunk = chunks_per_wii_group == 1 ? exception_lists_per_chunk * Wii::BLOCKS_PER_GROUP :
			                                                         Wii::BLOCKS_PER_GROUP / chunks_per_wii_group;

			const u64 chunks = AlignUp(blocks, blocks_per_chunk) / blocks_per_chunk;

			const u64 in_data_per_chunk = blocks_per_chunk * Wii::BLOCK_TOTAL_SIZE;
			const u64 out_data_per_chunk = blocks_per_chunk * Wii::BLOCK_DATA_SIZE;

			const std::size_t first_chunk = output_entries.size();

			const auto create_reuse_id = [&partition_entry, blocks, blocks_per_chunk](u8 value, bool encrypted,
			                                                                          u64 block) {
				const u64 size = std::min(blocks - block, blocks_per_chunk) * Wii::BLOCK_DATA_SIZE;
				return ReuseID{partition_entry.partition_key, size, encrypted, value};
			};

			const u8* parameters_data_end = parameters.data.data() + parameters.data.size();
			for (u64 i = 0; i < chunks; ++i) {
				const u64 block_index = i * blocks_per_chunk;

				OutputParametersEntry& entry = output_entries.emplace_back();
				std::optional<ReuseID>& reuse_id = entry.reuse_id;

				// Set this chunk as reusable if the encrypted data is AllSame
				const u8* data = parameters.data.data() + block_index * Wii::BLOCK_TOTAL_SIZE;
				if (AllSame(data, std::min(parameters_data_end, data + in_data_per_chunk)))
					reuse_id = create_reuse_id(parameters.data.front(), true, i * blocks_per_chunk);

				TryReuse(reusable_groups, reusable_groups_mutex, &entry);
				if (!entry.reused_group && reuse_id) {
					const auto it = std::find_if(output_entries.begin(), output_entries.begin() + i,
					                             [&](const OutputParametersEntry& e) { return e.reuse_id == reuse_id; });
					if (it != output_entries.begin() + i)
						entry.reused_group = it->reused_group;
				}
			}

			if (!std::all_of(output_entries.begin(), output_entries.end(),
			                 [](const OutputParametersEntry& entry) { return entry.reused_group.has_value(); })) {
				const u64 number_of_exception_lists = chunks_per_wii_group == 1 ? exception_lists_per_chunk : chunks;
				std::vector<std::vector<HashExceptionEntry>> exception_lists(number_of_exception_lists);

				for (u64 i = 0; i < groups; ++i) {
					const u64 offset_of_group = i * Wii::GROUP_TOTAL_SIZE;
					const u64 write_offset_of_group = i * Wii::GROUP_DATA_SIZE;

					const u64 blocks_in_this_group =
					    std::min<u64>(Wii::BLOCKS_PER_GROUP, blocks - i * Wii::BLOCKS_PER_GROUP);

					for (u32 j = 0; j < Wii::BLOCKS_PER_GROUP; ++j) {
						if (j < blocks_in_this_group) {
							const u64 offset_of_block = offset_of_group + j * Wii::BLOCK_TOTAL_SIZE;
							Wii::DecryptBlockData(parameters.data.data() + offset_of_block,
							                      state->decryption_buffer[j].data(), aes);
						} else {
							state->decryption_buffer[j].fill(0);
						}
					}

					Wii::HashGroup(state->decryption_buffer.data(), state->hash_buffer.data());

					for (u64 j = 0; j < blocks_in_this_group; ++j) {
						const u64 chunk_index = j / blocks_per_chunk;
						const u64 block_index_in_chunk = j % blocks_per_chunk;

						if (output_entries[chunk_index].reused_group)
							continue;

						const u64 exception_list_index = chunks_per_wii_group == 1 ? i : chunk_index;

						const u64 offset_of_block = offset_of_group + j * Wii::BLOCK_TOTAL_SIZE;
						const u64 hash_offset_of_block = block_index_in_chunk * Wii::BLOCK_HEADER_SIZE;

						Wii::HashBlock hashes;
						Wii::DecryptBlockHashes(parameters.data.data() + offset_of_block, &hashes, aes);

						const auto compare_hash = [&](std::size_t offset_in_block) {
							const u8* desired_hash = reinterpret_cast<u8*>(&hashes) + offset_in_block;
							const u8* computed_hash = reinterpret_cast<u8*>(&state->hash_buffer[j]) + offset_in_block;

							// A hash exception is stored either if there is a hash mismatch, or if this
							// chunk might get reused in a context where it is paired up (within a 2 MiB
							// Wii group) with chunks that are different from the chunks it currently is
							// paired up with, since that affects the recalculated hashes. Chunks which
							// have been marked as reusable at this point normally have zero matching
							// hashes anyway, so this shouldn't waste space.
							if ((chunks_per_wii_group != 1 && output_entries[chunk_index].reuse_id) ||
							    !std::equal(desired_hash, desired_hash + SHA1::DIGEST_LEN, computed_hash)) {
								const u64 hash_offset = hash_offset_of_block + offset_in_block;

								HashExceptionEntry& exception = exception_lists[exception_list_index].emplace_back();
								exception.offset = swap16(static_cast<u16>(hash_offset));
								std::memcpy(exception.hash.data(), desired_hash, SHA1::DIGEST_LEN);
							}
						};

						const auto compare_hashes = [&compare_hash](std::size_t offset, std::size_t size) {
							for (std::size_t l = 0; l < size; l += SHA1::DIGEST_LEN)
								// The std::min ensures we don't go beyond the end of HashBlock with
								// padding_2, which is 32 bytes long (not divisible by 20).
								compare_hash(offset + std::min(l, size - SHA1::DIGEST_LEN));
						};

						using HashBlock = Wii::HashBlock;
						compare_hashes(offsetof(HashBlock, h0), sizeof(HashBlock::h0));
						compare_hashes(offsetof(HashBlock, padding_0), sizeof(HashBlock::padding_0));
						compare_hashes(offsetof(HashBlock, h1), sizeof(HashBlock::h1));
						compare_hashes(offsetof(HashBlock, padding_1), sizeof(HashBlock::padding_1));
						compare_hashes(offsetof(HashBlock, h2), sizeof(HashBlock::h2));
						compare_hashes(offsetof(HashBlock, padding_2), sizeof(HashBlock::padding_2));
					}

					if constexpr (RVZ) {
						// We must not store junk efficiently for chunks that may get reused at a position
						// which has a different value of data_offset % Wii::BLOCK_TOTAL_SIZE
						const bool allow_junk_reuse = chunks_per_wii_group == 1;

						const u64 bytes_per_chunk = std::min(out_data_per_chunk, Wii::GROUP_DATA_SIZE);
						const u64 total_size = blocks_in_this_group * Wii::BLOCK_DATA_SIZE;
						const u64 data_offset = parameters.data_offset + write_offset_of_group;

						RVZPack(state->decryption_buffer[0].data(), output_entries.data() + first_chunk, bytes_per_chunk,
						        static_cast<std::size_t>(chunks), total_size, data_offset, groups > 1, allow_junk_reuse,
						        compression, file_system);
					} else {
						const u8* in_ptr = state->decryption_buffer[0].data();
						for (u64 j = 0; j < chunks; ++j) {
							OutputParametersEntry& entry = output_entries[first_chunk + j];

							if (!entry.reused_group) {
								const u64 bytes_left = (blocks - j * blocks_per_chunk) * Wii::BLOCK_DATA_SIZE;
								const u64 bytes_to_write_total = std::min(out_data_per_chunk, bytes_left);

								if (i == 0)
									entry.main_data.resize(bytes_to_write_total);

								// (Dolphin uses bytes_to_write_total here, which overruns main_data when a chunk of
								// more than 2 MiB ends in a partial group)
								const u64 bytes_to_write =
								    std::min(bytes_to_write_total - write_offset_of_group, Wii::GROUP_DATA_SIZE);

								std::memcpy(entry.main_data.data() + write_offset_of_group, in_ptr, bytes_to_write);

								// Set this chunk as reusable if the decrypted data is AllSame.
								// It also must lack exceptions, but that is checked later
								if (i == 0 && !entry.reuse_id) {
									if (AllSame(in_ptr, in_ptr + bytes_to_write))
										entry.reuse_id = create_reuse_id(*in_ptr, false, j * blocks_per_chunk);
								} else {
									if (entry.reuse_id && !entry.reuse_id->encrypted &&
									    (!AllSame(in_ptr, in_ptr + bytes_to_write) || entry.reuse_id->value != *in_ptr)) {
										entry.reuse_id.reset();
									}
								}
							}

							in_ptr += out_data_per_chunk;
						}
					}
				}

				for (std::size_t i = 0; i < exception_lists.size(); ++i) {
					OutputParametersEntry& entry = output_entries[chunks_per_wii_group == 1 ? 0 : i];
					if (entry.reused_group)
						continue;

					const std::vector<HashExceptionEntry>& in = exception_lists[i];
					std::vector<u8>& out = entry.exception_lists;

					const u16 exceptions = swap16(static_cast<u16>(in.size()));
					PushBack(&out, exceptions);
					for (const HashExceptionEntry& exception : in)
						PushBack(&out, exception);
				}

				for (u64 i = 0; i < output_entries.size(); ++i) {
					OutputParametersEntry& entry = output_entries[i];

					// If this chunk was set as reusable because the decrypted data is AllSame, but it
					// has exceptions, unmark it as reusable
					if (entry.reuse_id && !entry.reuse_id->encrypted && !AllZero(entry.exception_lists))
						entry.reuse_id.reset();
				}
			}
		}

		for (OutputParametersEntry& entry : output_entries) {
			TryReuse(reusable_groups, reusable_groups_mutex, &entry);
			if (entry.reused_group)
				continue;

			// Special case - a compressed size of zero is treated by WIA as meaning the data is all zeroes
			if (entry.reuse_id && !entry.reuse_id->encrypted && entry.reuse_id->value == 0) {
				entry.exception_lists.clear();
				entry.main_data.clear();
				entry.rvz_packed_size = 0;
				entry.compressed = false;
				continue;
			}

			const auto pad_exception_lists = [&entry] {
				while (entry.exception_lists.size() % 4 != 0)
					entry.exception_lists.push_back(0);
			};

			if (state->compressor) {
				if (!state->compressor->Start(entry.exception_lists.size() + entry.main_data.size()))
					return {ConversionResultCode::InternalError};
			}

			if (!entry.exception_lists.empty()) {
				if (compressed_exception_lists && state->compressor) {
					if (!state->compressor->Compress(entry.exception_lists.data(), entry.exception_lists.size()))
						return {ConversionResultCode::InternalError};
				} else {
					if (!compressed_exception_lists)
						pad_exception_lists();

					if (state->compressor) {
						if (!state->compressor->AddPrecedingDataOnlyForPurgeHashing(entry.exception_lists.data(),
						                                                           entry.exception_lists.size())) {
							return {ConversionResultCode::InternalError};
						}
					}
				}
			}

			if (state->compressor) {
				if (!state->compressor->Compress(entry.main_data.data(), entry.main_data.size()))
					return {ConversionResultCode::InternalError};
				if (!state->compressor->End())
					return {ConversionResultCode::InternalError};
			}

			bool compressed = !!state->compressor;
			if constexpr (RVZ) {
				std::size_t uncompressed_size = entry.main_data.size();
				if (compressed_exception_lists)
					uncompressed_size += AlignUp(entry.exception_lists.size(), 4);

				compressed = state->compressor && state->compressor->GetSize() < uncompressed_size;
				entry.compressed = compressed;

				if (!compressed)
					pad_exception_lists();
			}

			if (compressed) {
				const u8* data = state->compressor->GetData();
				const std::size_t size = state->compressor->GetSize();

				entry.main_data.resize(size);
				std::copy_n(data, size, entry.main_data.data());

				if (compressed_exception_lists)
					entry.exception_lists.clear();
			}
		}

		return {ConversionResultCode::Success,
		        OutputParameters{std::move(output_entries), parameters.bytes_read, parameters.group_index}};
	}

	static ConversionResultCode WriteEntries(std::vector<OutputParametersEntry>* entries, Output* outfile,
	                                         std::map<ReuseID, GroupEntry>* reusable_groups,
	                                         std::mutex* reusable_groups_mutex, GroupEntry* group_entry,
	                                         u64* bytes_written) {
		for (OutputParametersEntry& entry : *entries) {
			TryReuse(reusable_groups, reusable_groups_mutex, &entry);
			if (entry.reused_group) {
				*group_entry = *entry.reused_group;
				++group_entry;
				continue;
			}

			if (*bytes_written >> 2 > std::numeric_limits<u32>::max())
				return ConversionResultCode::InternalError;

			group_entry->data_offset = swap32(static_cast<u32>(*bytes_written >> 2));

			u32 data_size = static_cast<u32>(entry.exception_lists.size() + entry.main_data.size());
			if constexpr (RVZ) {
				data_size = (data_size & 0x7FFFFFFF) | (static_cast<u32>(entry.compressed) << 31);
				group_entry->rvz_packed_size = swap32(static_cast<u32>(entry.rvz_packed_size));
			}
			group_entry->data_size = swap32(data_size);

			if (!outfile->Write(entry.exception_lists))
				return ConversionResultCode::WriteFailed;
			if (!outfile->Write(entry.main_data))
				return ConversionResultCode::WriteFailed;

			*bytes_written += entry.exception_lists.size() + entry.main_data.size();

			if (entry.reuse_id) {
				std::lock_guard<std::mutex> guard(*reusable_groups_mutex);
				reusable_groups->emplace(*entry.reuse_id, *group_entry);
			}

			if (!PadTo4(outfile, bytes_written))
				return ConversionResultCode::WriteFailed;

			++group_entry;
		}

		return ConversionResultCode::Success;
	}

	static bool WriteHeader(Output* file, const u8* data, std::size_t size, u64 upper_bound, u64* bytes_written,
	                        u64* offset_out) {
		// The first part of the check prevents this from running more than once. If *bytes_written
		// is past the upper bound, we are already at the end of the file.
		if (*bytes_written <= upper_bound && *bytes_written + size > upper_bound) {
			// Headers did not fit in the allocated space. Write them to the end of the file instead.
			file->position = file->size;
			*bytes_written = file->position;
		}

		*offset_out = *bytes_written;
		if (!file->Write(data, size))
			return false;
		*bytes_written += size;
		return PadTo4(file, bytes_written);
	}

	static ConversionResultCode Convert(BlobReader* infile, const Volume* infile_volume, Sink* sink,
	                                    CompressionType compression_type, int compression_level, int chunk_size,
	                                    int threads, const ProgressCallback& callback) {
		Output out{sink};
		Output* outfile = &out;

		const u64 iso_size = infile->GetDataSize();
		const u64 chunks_per_wii_group = std::max<u64>(1, Wii::GROUP_TOTAL_SIZE / chunk_size);
		const u64 exception_lists_per_chunk = std::max<u64>(1, chunk_size / Wii::GROUP_TOTAL_SIZE);
		const bool compressed_exception_lists = compression_type > CompressionType::Purge;

		u64 bytes_read = 0;
		u64 bytes_written = 0;
		std::size_t groups_processed = 0;

		WIAHeader1 header_1{};
		WIAHeader2 header_2{};

		std::vector<PartitionEntry> partition_entries;
		std::vector<RawDataEntry> raw_data_entries;
		std::vector<GroupEntry> group_entries;

		u32 total_groups;
		std::vector<DataEntry> data_entries;

		const std::unique_ptr<FileSystem> non_partition_file_system =
		    infile_volume && infile_volume->GetType() == DiscType::GameCube ? infile_volume->CreateFileSystem(nullptr) :
		                                                                     nullptr;
		std::vector<std::unique_ptr<FileSystem>> partition_file_systems;

		const ConversionResultCode set_up_data_entries_result =
		    SetUpDataEntriesForWriting(infile_volume, chunk_size, iso_size, &total_groups, &partition_entries,
		                               &raw_data_entries, &data_entries, &partition_file_systems);
		if (set_up_data_entries_result != ConversionResultCode::Success)
			return set_up_data_entries_result;

		group_entries.resize(total_groups);

		const std::size_t partition_entries_size = partition_entries.size() * sizeof(PartitionEntry);
		const std::size_t raw_data_entries_size = raw_data_entries.size() * sizeof(RawDataEntry);
		const std::size_t group_entries_size = group_entries.size() * sizeof(GroupEntry);

		// An estimate for how much space will be taken up by headers. This much space is reserved at
		// the beginning of the file; if the headers don't fit, they go to the end of the file.
		const u64 headers_size_upper_bound = [&] {
			// 0x100 is added to account for compression overhead (in particular for Purge).
			u64 upper_bound =
			    sizeof(WIAHeader1) + sizeof(WIAHeader2) + partition_entries_size + raw_data_entries_size + 0x100;

			// Compared to WIA, RVZ adds an extra member to the GroupEntry struct. This added data
			// usually compresses well, so a compression ratio of 9 / 16 or better is assumed.
			if (RVZ && compression_type > CompressionType::Purge)
				upper_bound += static_cast<u64>(group_entries_size) * 9 / 16;
			else
				upper_bound += group_entries_size;

			// This alignment is also somewhat arbitrary.
			return AlignUp(upper_bound, Wii::BLOCK_TOTAL_SIZE);
		}();

		std::vector<u8> buffer;

		buffer.resize(headers_size_upper_bound);
		if (!outfile->Write(buffer))
			return ConversionResultCode::WriteFailed;
		bytes_written = headers_size_upper_bound;

		if (!infile->Read(0, header_2.disc_header.size(), header_2.disc_header.data()))
			return ConversionResultCode::ReadFailed;
		// bytes_read is intentionally not incremented here, since these bytes will be read again

		std::map<ReuseID, GroupEntry> reusable_groups;
		std::mutex reusable_groups_mutex;

		std::atomic<u64> bytes_done{0};

		const auto set_up_compress_thread_state = [&](CompressThreadState* state) {
			state->compressor = CreateCompressor(compression_type, compression_level);
			return ConversionResultCode::Success;
		};

		const auto process_and_compress = [&](CompressThreadState* state, CompressParameters parameters) {
			const DataEntry& data_entry = *parameters.data_entry;
			const FileSystem* file_system = data_entry.is_partition ? partition_file_systems[data_entry.index].get() :
			                                                          non_partition_file_system.get();

			const bool compression = compression_type != CompressionType::None;

			return ProcessAndCompress(state, std::move(parameters), partition_entries, file_system, &reusable_groups,
			                          &reusable_groups_mutex, chunks_per_wii_group, exception_lists_per_chunk,
			                          compressed_exception_lists, compression);
		};

		const auto output = [&](OutputParameters parameters) {
			const ConversionResultCode result =
			    WriteEntries(&parameters.entries, outfile, &reusable_groups, &reusable_groups_mutex,
			                 &group_entries[parameters.group_index], &bytes_written);
			if (result == ConversionResultCode::Success)
				bytes_done.store(parameters.bytes_read);
			return result;
		};

		const auto report = [&] {
			return !callback || callback(bytes_done.load(), iso_size);
		};

		{
			MultithreadedCompressor<CompressThreadState, CompressParameters, OutputParameters> mt_compressor(
			    set_up_compress_thread_state, process_and_compress, output,
			    threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency());

			for (const DataEntry& data_entry : data_entries) {
				u32 first_group;
				u32 last_group;

				u64 data_offset;
				u64 data_size;

				u64 data_offset_in_partition;

				if (data_entry.is_partition) {
					const PartitionEntry& partition_entry = partition_entries[data_entry.index];
					const PartitionDataEntry& partition_data_entry =
					    partition_entry.data_entries[data_entry.partition_data_index];

					first_group = swap32(partition_data_entry.group_index);
					last_group = first_group + swap32(partition_data_entry.number_of_groups);

					const u32 first_sector = swap32(partition_data_entry.first_sector);
					data_offset = first_sector * Wii::BLOCK_TOTAL_SIZE;
					data_size = swap32(partition_data_entry.number_of_sectors) * Wii::BLOCK_TOTAL_SIZE;

					const u32 block_in_partition = first_sector - swap32(partition_entry.data_entries[0].first_sector);
					data_offset_in_partition = block_in_partition * Wii::BLOCK_DATA_SIZE;
				} else {
					const RawDataEntry& raw_data_entry = raw_data_entries[data_entry.index];

					first_group = swap32(raw_data_entry.group_index);
					last_group = first_group + swap32(raw_data_entry.number_of_groups);

					data_offset = swap64(raw_data_entry.data_offset);
					data_size = swap64(raw_data_entry.data_size);

					const u64 skipped_data = data_offset % Wii::BLOCK_TOTAL_SIZE;
					data_offset -= skipped_data;
					data_size += skipped_data;

					data_offset_in_partition = data_offset;
				}

				if (groups_processed != first_group || bytes_read != data_offset)
					return ConversionResultCode::InternalError;

				while (groups_processed < last_group) {
					const ConversionResultCode status = mt_compressor.GetStatus();
					if (status != ConversionResultCode::Success)
						return status;
					if (!report()) {
						mt_compressor.SetError(ConversionResultCode::Canceled);
						return ConversionResultCode::Canceled;
					}

					u64 bytes_to_read = chunk_size;
					if (data_entry.is_partition)
						bytes_to_read = std::max<u64>(bytes_to_read, Wii::GROUP_TOTAL_SIZE);
					bytes_to_read = std::min<u64>(bytes_to_read, data_offset + data_size - bytes_read);

					buffer.resize(static_cast<std::size_t>(bytes_to_read));
					if (!infile->Read(bytes_read, bytes_to_read, buffer.data())) {
						mt_compressor.SetError(ConversionResultCode::ReadFailed);
						return ConversionResultCode::ReadFailed;
					}
					bytes_read += bytes_to_read;

					mt_compressor.CompressAndWrite(
					    CompressParameters{buffer, &data_entry, data_offset_in_partition, bytes_read, groups_processed});

					data_offset += bytes_to_read;
					data_size -= bytes_to_read;

					if (data_entry.is_partition)
						data_offset_in_partition += bytes_to_read / Wii::BLOCK_TOTAL_SIZE * Wii::BLOCK_DATA_SIZE;
					else
						data_offset_in_partition += bytes_to_read;

					groups_processed += static_cast<std::size_t>(AlignUp(bytes_to_read, chunk_size) / chunk_size);
				}

				if (data_size != 0)
					return ConversionResultCode::InternalError;
			}

			if (groups_processed != total_groups || bytes_read != iso_size)
				return ConversionResultCode::InternalError;

			mt_compressor.Shutdown();

			const ConversionResultCode status = mt_compressor.GetStatus();
			if (status != ConversionResultCode::Success)
				return status;
		}
		if (!report())
			return ConversionResultCode::Canceled;

		std::unique_ptr<Compressor> compressor = CreateCompressor(compression_type, compression_level,
		                                                          header_2.compressor_data, &header_2.compressor_data_size);

		const std::optional<std::vector<u8>> compressed_raw_data_entries =
		    Compress(compressor.get(), reinterpret_cast<u8*>(raw_data_entries.data()), raw_data_entries_size);
		if (!compressed_raw_data_entries)
			return ConversionResultCode::InternalError;

		const std::optional<std::vector<u8>> compressed_group_entries =
		    Compress(compressor.get(), reinterpret_cast<u8*>(group_entries.data()), group_entries_size);
		if (!compressed_group_entries)
			return ConversionResultCode::InternalError;

		bytes_written = sizeof(WIAHeader1) + sizeof(WIAHeader2);
		outfile->position = sizeof(WIAHeader1) + sizeof(WIAHeader2);

		u64 partition_entries_offset;
		if (!WriteHeader(outfile, reinterpret_cast<u8*>(partition_entries.data()), partition_entries_size,
		                 headers_size_upper_bound, &bytes_written, &partition_entries_offset)) {
			return ConversionResultCode::WriteFailed;
		}

		u64 raw_data_entries_offset;
		if (!WriteHeader(outfile, compressed_raw_data_entries->data(), compressed_raw_data_entries->size(),
		                 headers_size_upper_bound, &bytes_written, &raw_data_entries_offset)) {
			return ConversionResultCode::WriteFailed;
		}

		u64 group_entries_offset;
		if (!WriteHeader(outfile, compressed_group_entries->data(), compressed_group_entries->size(),
		                 headers_size_upper_bound, &bytes_written, &group_entries_offset)) {
			return ConversionResultCode::WriteFailed;
		}

		u32 disc_type = 0;
		if (infile_volume)
			disc_type = static_cast<u32>(infile_volume->GetType());

		header_2.disc_type = swap32(disc_type);
		header_2.compression_type = swap32(static_cast<u32>(compression_type));
		header_2.compression_level = static_cast<s32>(swap32(static_cast<u32>(compression_level)));
		header_2.chunk_size = swap32(static_cast<u32>(chunk_size));

		header_2.number_of_partition_entries = swap32(static_cast<u32>(partition_entries.size()));
		header_2.partition_entry_size = swap32(sizeof(PartitionEntry));
		header_2.partition_entries_offset = swap64(partition_entries_offset);

		header_2.partition_entries_hash = SHA1::CalculateDigest(partition_entries.data(), partition_entries_size);

		header_2.number_of_raw_data_entries = swap32(static_cast<u32>(raw_data_entries.size()));
		header_2.raw_data_entries_offset = swap64(raw_data_entries_offset);
		header_2.raw_data_entries_size = swap32(static_cast<u32>(compressed_raw_data_entries->size()));

		header_2.number_of_group_entries = swap32(static_cast<u32>(group_entries.size()));
		header_2.group_entries_offset = swap64(group_entries_offset);
		header_2.group_entries_size = swap32(static_cast<u32>(compressed_group_entries->size()));

		header_1.magic = RVZ ? RVZ_MAGIC : WIA_MAGIC;
		header_1.version = swap32(RVZ ? RVZ_VERSION : WIA_VERSION);
		header_1.version_compatible = swap32(RVZ ? RVZ_VERSION_WRITE_COMPATIBLE : WIA_VERSION_WRITE_COMPATIBLE);
		header_1.header_2_size = swap32(sizeof(WIAHeader2));
		header_1.header_2_hash = SHA1::CalculateDigest(&header_2, sizeof(header_2));
		header_1.iso_file_size = swap64(infile->GetDataSize());
		header_1.wia_file_size = swap64(outfile->size);
		header_1.header_1_hash = SHA1::CalculateDigest(&header_1, offsetof(WIAHeader1, header_1_hash));

		outfile->position = 0;
		if (!outfile->Write(&header_1, sizeof(header_1)))
			return ConversionResultCode::WriteFailed;
		if (!outfile->Write(&header_2, sizeof(header_2)))
			return ConversionResultCode::WriteFailed;

		if (!sink->Finish(outfile->size))
			return ConversionResultCode::WriteFailed;

		return ConversionResultCode::Success;
	}
};

}  // namespace

std::unique_ptr<WIAReader> WIAReader::Create(std::shared_ptr<Source> source) {
	u32 magic = 0;
	if (!source || !source->ReadAt(0, &magic, sizeof(magic)))
		return nullptr;
	if (magic == WIA_MAGIC) {
		auto reader = std::make_unique<WIARVZFileReader<false>>(std::move(source));
		return reader->IsValid() ? std::move(reader) : nullptr;
	}
	if (magic == RVZ_MAGIC) {
		auto reader = std::make_unique<WIARVZFileReader<true>>(std::move(source));
		return reader->IsValid() ? std::move(reader) : nullptr;
	}
	return nullptr;
}

const char* ValidateConvertOptions(const ConvertOptions& options) {
	const u32 chunk_size = static_cast<u32>(options.chunk_size);
	if (options.chunk_size <= 0)
		return "the chunk size must be positive";
	if (options.rvz) {
		const bool power_of_two = (chunk_size & (chunk_size - 1)) == 0;
		if ((chunk_size < Wii::BLOCK_TOTAL_SIZE || !power_of_two) && chunk_size % Wii::GROUP_TOTAL_SIZE != 0)
			return "RVZ chunk sizes are powers of two from 32 KiB, or multiples of 2 MiB";
		if (options.compression == CompressionType::Purge)
			return "RVZ doesn't support Purge";
	} else {
		if (chunk_size % Wii::GROUP_TOTAL_SIZE != 0)
			return "WIA chunk sizes are multiples of 2 MiB";
		if (options.compression == CompressionType::Zstd)
			return "WIA doesn't support Zstandard";
	}
	if (options.compression > CompressionType::Zstd)
		return "unknown compression method";
	const auto [min_level, max_level] = GetAllowedCompressionLevels(options.compression);
	if (min_level <= max_level && (options.compression_level < min_level || options.compression_level > max_level))
		return "compression level out of range for this method";
	return nullptr;
}

ConversionResultCode ConvertToWIAOrRVZ(BlobReader* infile, Sink* outfile, const ConvertOptions& options,
                                       const ProgressCallback& progress) {
	if (ValidateConvertOptions(options))
		return ConversionResultCode::InternalError;

	const std::unique_ptr<Volume> volume = Volume::Create(infile);
	if (!outfile->Begin(0))
		return ConversionResultCode::WriteFailed;

	const auto convert = options.rvz ? Writer<true>::Convert : Writer<false>::Convert;
	return convert(infile, volume.get(), outfile, options.compression, options.compression_level, options.chunk_size,
	               options.threads, progress);
}

}  // namespace irvz
