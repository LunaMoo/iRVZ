// iRVZ - WIA/RVZ disc image library
// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ported from Dolphin's Source/Core/DiscIO/WIACompression.cpp. The LZMA and LZMA2 codecs use the
// LZMA SDK instead of liblzma: WIA stores raw LZMA streams (with an end marker) and raw LZMA2
// streams, which both libraries read and write.

#include "compression.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <bzlib.h>
#include <zstd.h>

#include "lzma/C/Lzma2Dec.h"
#include "lzma/C/Lzma2Enc.h"
#include "lzma/C/LzmaDec.h"
#include "lzma/C/LzmaEnc.h"

#include "crypto.h"
#include "lfg.h"

#ifndef IRVZ_EXTERNAL_BZIP2
// bzip2 is built with BZ_NO_STDIO, which leaves this to the program.
extern "C" void bz_internal_error(int) {
	std::abort();
}
#endif

namespace irvz {

namespace {

void* SzAlloc(ISzAllocPtr, size_t size) {
	return std::malloc(size);
}
void SzFree(ISzAllocPtr, void* address) {
	std::free(address);
}
const ISzAlloc g_alloc = {SzAlloc, SzFree};

u32 LZMA2DictionarySize(u8 p) {
	return (static_cast<u32>(2) | (p & 1)) << (p / 2 + 11);
}

// The dictionary sizes of liblzma's presets 0-9, which is what Dolphin writes.
u32 LZMADictionarySize(int level) {
	static const u8 shift[10] = {18, 20, 21, 22, 22, 23, 23, 24, 25, 26};
	return u32(1) << shift[std::clamp(level, 0, 9)];
}

template <typename T>
T SaturatingCast(std::size_t x) {
	return x > static_cast<std::size_t>(static_cast<T>(-1)) ? static_cast<T>(-1) : static_cast<T>(x);
}

// ---- Decompressors ----------------------------------------------------------------------------

class NoneDecompressor final : public Decompressor {
public:
	bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) override {
		const std::size_t length = std::min(in.bytes_written - *in_bytes_read, out->data.size() - out->bytes_written);
		if (length)
			std::memcpy(out->data.data() + out->bytes_written, in.data.data() + *in_bytes_read, length);
		*in_bytes_read += length;
		out->bytes_written += length;
		m_done = in.data.size() == *in_bytes_read;
		return true;
	}
};

// Assumes that more bytes won't be added to in once in.bytes_written == in.data.size() and that
// *in_bytes_read initially is the size of the exception lists.
class PurgeDecompressor final : public Decompressor {
public:
	explicit PurgeDecompressor(u64 decompressed_size) : m_decompressed_size(decompressed_size) {}

	bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) override {
		if (!m_started) {
			// Include the exception lists in the SHA-1 calculation (but not in the compression...)
			m_sha1.Update(in.data.data(), *in_bytes_read);
			m_started = true;
		}

		while (!m_done && in.bytes_written != *in_bytes_read &&
		       (m_segment_bytes_written < sizeof(m_segment) || out->data.size() != out->bytes_written)) {
			if (m_segment_bytes_written == 0 && *in_bytes_read == in.data.size() - SHA1::DIGEST_LEN) {
				const std::size_t zeroes_to_write = std::min<std::size_t>(m_decompressed_size - m_out_bytes_written,
				                                                          out->data.size() - out->bytes_written);
				std::memset(out->data.data() + out->bytes_written, 0, zeroes_to_write);
				out->bytes_written += zeroes_to_write;
				m_out_bytes_written += zeroes_to_write;

				if (m_out_bytes_written == m_decompressed_size && in.bytes_written == in.data.size()) {
					const SHA1::Digest actual_hash = m_sha1.Finish();
					SHA1::Digest expected_hash;
					std::memcpy(expected_hash.data(), in.data.data() + *in_bytes_read, expected_hash.size());
					*in_bytes_read += expected_hash.size();
					m_done = true;
					if (actual_hash != expected_hash)
						return false;
				}
				return true;
			}

			if (m_segment_bytes_written < sizeof(m_segment)) {
				const std::size_t bytes_to_copy =
				    std::min(in.bytes_written - *in_bytes_read, sizeof(m_segment) - m_segment_bytes_written);
				std::memcpy(reinterpret_cast<u8*>(&m_segment) + m_segment_bytes_written, in.data.data() + *in_bytes_read,
				            bytes_to_copy);
				m_sha1.Update(in.data.data() + *in_bytes_read, bytes_to_copy);
				*in_bytes_read += bytes_to_copy;
				m_segment_bytes_written += bytes_to_copy;
			}

			if (m_segment_bytes_written < sizeof(m_segment))
				return true;

			const std::size_t offset = swap32(m_segment.offset);
			const std::size_t size = swap32(m_segment.size);

			if (m_out_bytes_written < offset) {
				const std::size_t zeroes_to_write =
				    std::min(offset - m_out_bytes_written, out->data.size() - out->bytes_written);
				std::memset(out->data.data() + out->bytes_written, 0, zeroes_to_write);
				out->bytes_written += zeroes_to_write;
				m_out_bytes_written += zeroes_to_write;
			}

			if (m_out_bytes_written >= offset && m_out_bytes_written < offset + size) {
				const std::size_t bytes_to_copy =
				    std::min(std::min(offset + size - m_out_bytes_written, out->data.size() - out->bytes_written),
				             in.bytes_written - *in_bytes_read);
				std::memcpy(out->data.data() + out->bytes_written, in.data.data() + *in_bytes_read, bytes_to_copy);
				m_sha1.Update(in.data.data() + *in_bytes_read, bytes_to_copy);
				*in_bytes_read += bytes_to_copy;
				out->bytes_written += bytes_to_copy;
				m_out_bytes_written += bytes_to_copy;
			}

			if (m_out_bytes_written >= offset + size)
				m_segment_bytes_written = 0;
		}

		return true;
	}

private:
	const u64 m_decompressed_size;
	PurgeSegment m_segment = {};
	std::size_t m_segment_bytes_written = 0;
	std::size_t m_out_bytes_written = 0;
	bool m_started = false;
	SHA1::Context m_sha1;
};

class Bzip2Decompressor final : public Decompressor {
public:
	~Bzip2Decompressor() override {
		if (m_started)
			BZ2_bzDecompressEnd(&m_stream);
	}

	bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) override {
		if (!m_started) {
			if (BZ2_bzDecompressInit(&m_stream, 0, 0) != BZ_OK)
				return false;
			m_started = true;
		}

		char* const in_ptr = reinterpret_cast<char*>(const_cast<u8*>(in.data.data() + *in_bytes_read));
		m_stream.next_in = in_ptr;
		m_stream.avail_in = SaturatingCast<unsigned>(in.bytes_written - *in_bytes_read);

		char* const out_ptr = reinterpret_cast<char*>(out->data.data() + out->bytes_written);
		m_stream.next_out = out_ptr;
		m_stream.avail_out = SaturatingCast<unsigned>(out->data.size() - out->bytes_written);

		const int result = BZ2_bzDecompress(&m_stream);

		*in_bytes_read += m_stream.next_in - in_ptr;
		out->bytes_written += m_stream.next_out - out_ptr;

		m_done = result == BZ_STREAM_END;
		return result == BZ_OK || result == BZ_STREAM_END;
	}

private:
	bz_stream m_stream = {};
	bool m_started = false;
};

class LZMADecompressor final : public Decompressor {
public:
	LZMADecompressor(bool lzma2, const u8* filter_options, std::size_t filter_options_size, u64 max_output)
	    : m_lzma2(lzma2) {
		// No stream needs a window bigger than all of its output, so the dictionary is capped at
		// that: decoding a 128 KiB chunk then doesn't allocate a 64 MiB dictionary.
		const u64 needed = std::max<u64>(max_output, 4096);
		if (!lzma2 && filter_options_size == 5) {
			if (filter_options[0] >= 9 * 5 * 5)
				return;
			// The dictionary size is stored as a 32-bit little endian unsigned integer
			u32 dict_size = u32(filter_options[1]) | (u32(filter_options[2]) << 8) | (u32(filter_options[3]) << 16) |
			                (u32(filter_options[4]) << 24);
			if (dict_size > needed)
				dict_size = static_cast<u32>(needed);
			const u8 props[5] = {filter_options[0], static_cast<u8>(dict_size), static_cast<u8>(dict_size >> 8),
			                     static_cast<u8>(dict_size >> 16), static_cast<u8>(dict_size >> 24)};
			LzmaDec_CONSTRUCT(&m_lzma);
			m_valid = LzmaDec_Allocate(&m_lzma, props, LZMA_PROPS_SIZE, &g_alloc) == SZ_OK;
			if (m_valid)
				LzmaDec_Init(&m_lzma);
		} else if (lzma2 && filter_options_size == 1) {
			u8 prop = filter_options[0];
			if (prop > 40)
				return;
			while (prop > 0 && LZMA2DictionarySize(prop - 1) >= needed)
				--prop;
			Lzma2Dec_CONSTRUCT(&m_lzma2_dec);
			m_valid = Lzma2Dec_Allocate(&m_lzma2_dec, prop, &g_alloc) == SZ_OK;
			if (m_valid)
				Lzma2Dec_Init(&m_lzma2_dec);
		}
	}

	~LZMADecompressor() override {
		if (!m_valid)
			return;
		if (m_lzma2)
			Lzma2Dec_Free(&m_lzma2_dec, &g_alloc);
		else
			LzmaDec_Free(&m_lzma, &g_alloc);
	}

	bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) override {
		if (!m_valid)
			return false;

		SizeT in_size = in.bytes_written - *in_bytes_read;
		SizeT out_size = out->data.size() - out->bytes_written;
		ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
		const SRes result =
		    m_lzma2 ? Lzma2Dec_DecodeToBuf(&m_lzma2_dec, out->data.data() + out->bytes_written, &out_size,
		                                   in.data.data() + *in_bytes_read, &in_size, LZMA_FINISH_ANY, &status) :
		              LzmaDec_DecodeToBuf(&m_lzma, out->data.data() + out->bytes_written, &out_size,
		                                  in.data.data() + *in_bytes_read, &in_size, LZMA_FINISH_ANY, &status);
		*in_bytes_read += in_size;
		out->bytes_written += out_size;

		m_done = status == LZMA_STATUS_FINISHED_WITH_MARK;
		if (result != SZ_OK)
			return false;

		// With the output buffer full, the SDK (unlike liblzma) doesn't look at the end marker that
		// may follow. Check for it here without disturbing the decoder if it isn't there (the output
		// may only be full for now, e.g. while RVZ packing hands out one segment at a time).
		if (!m_done && out->bytes_written == out->data.size() && *in_bytes_read < in.bytes_written) {
			const u8* next = in.data.data() + *in_bytes_read;
			if (m_lzma2) {
				// Between chunks, control byte 0 ends the stream. That's state 0 (LZMA2_STATE_CONTROL),
				// or state 7 (LZMA2_STATE_DATA_CONT) with nothing left of the current LZMA chunk.
				const bool between_chunks =
				    m_lzma2_dec.state == 0 ||
				    (m_lzma2_dec.state == 7 && m_lzma2_dec.unpackSize == 0 && m_lzma2_dec.packSize == 0);
				if (between_chunks && *next == 0) {
					++*in_bytes_read;
					m_done = true;
				}
			} else {
				// Probe on a copy of the decoder. If the next symbol is a match that isn't the end marker,
				// decoding it updates the (shared) probability table, so that is saved and restored.
				CLzmaDec probe = m_lzma;
				m_saved_probs.assign(m_lzma.probs, m_lzma.probs + m_lzma.numProbs);
				SizeT probe_in = in.bytes_written - *in_bytes_read;
				SizeT probe_out = 0;
				u8 dummy;
				if (LzmaDec_DecodeToBuf(&probe, &dummy, &probe_out, next, &probe_in, LZMA_FINISH_END, &status) == SZ_OK &&
				    status == LZMA_STATUS_FINISHED_WITH_MARK) {
					m_lzma = probe;
					*in_bytes_read += probe_in;
					m_done = true;
				} else {
					std::copy(m_saved_probs.begin(), m_saved_probs.end(), m_lzma.probs);
				}
			}
		}
		return true;
	}

private:
	bool m_lzma2;
	bool m_valid = false;
	CLzmaDec m_lzma;
	CLzma2Dec m_lzma2_dec;
	std::vector<CLzmaProb> m_saved_probs;
};

class ZstdDecompressor final : public Decompressor {
public:
	ZstdDecompressor() : m_stream(ZSTD_createDStream()) {}
	~ZstdDecompressor() override { ZSTD_freeDStream(m_stream); }

	bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) override {
		if (!m_stream)
			return false;
		ZSTD_inBuffer in_buffer{in.data.data(), in.bytes_written, *in_bytes_read};
		ZSTD_outBuffer out_buffer{out->data.data(), out->data.size(), out->bytes_written};
		const std::size_t result = ZSTD_decompressStream(m_stream, &out_buffer, &in_buffer);
		*in_bytes_read = in_buffer.pos;
		out->bytes_written = out_buffer.pos;
		m_done = result == 0;
		return !ZSTD_isError(result);
	}

private:
	ZSTD_DStream* m_stream;
};

class RVZPackDecompressor final : public Decompressor {
public:
	RVZPackDecompressor(std::unique_ptr<Decompressor> decompressor, DecompressionBuffer decompressed, u64 data_offset,
	                    u32 rvz_packed_size)
	    : m_decompressor(std::move(decompressor)), m_decompressed(std::move(decompressed)),
	      m_data_offset(data_offset), m_rvz_packed_size(rvz_packed_size) {
		m_bytes_read = m_decompressed.bytes_written;
	}

	bool Decompress(const DecompressionBuffer& in, DecompressionBuffer* out, std::size_t* in_bytes_read) override {
		while (out->data.size() != out->bytes_written && !Done()) {
			if (m_size == 0) {
				if (m_decompressed.bytes_written == m_decompressed_bytes_read) {
					m_decompressed.data.resize(sizeof(u32));
					m_decompressed.bytes_written = 0;
					m_decompressed_bytes_read = 0;
				}

				int result = ReadToDecompressed(in, in_bytes_read, m_decompressed_bytes_read, sizeof(u32));
				if (result >= 0)
					return result != 0;

				const u32 size = swap32(m_decompressed.data.data() + m_decompressed_bytes_read);

				m_junk = size & 0x80000000;
				if (m_junk) {
					constexpr std::size_t SEED_SIZE = LaggedFibonacciGenerator::SEED_SIZE * sizeof(u32);
					constexpr std::size_t BLOCK_SIZE = 0x8000;

					result = ReadToDecompressed(in, in_bytes_read, m_decompressed_bytes_read + sizeof(u32), SEED_SIZE);
					if (result >= 0)
						return result != 0;

					m_lfg.SetSeed(m_decompressed.data.data() + m_decompressed_bytes_read + sizeof(u32));
					m_lfg.Forward(m_data_offset % BLOCK_SIZE);

					m_decompressed_bytes_read += SEED_SIZE;
				}

				m_decompressed_bytes_read += sizeof(u32);
				m_size = size & 0x7FFFFFFF;
			}

			std::size_t bytes_to_write = std::min<std::size_t>(m_size, out->data.size() - out->bytes_written);
			if (m_junk) {
				m_lfg.GetBytes(bytes_to_write, out->data.data() + out->bytes_written);
				out->bytes_written += bytes_to_write;
			} else if (m_decompressed.bytes_written != m_decompressed_bytes_read) {
				bytes_to_write = std::min(bytes_to_write, m_decompressed.bytes_written - m_decompressed_bytes_read);
				std::memcpy(out->data.data() + out->bytes_written, m_decompressed.data.data() + m_decompressed_bytes_read,
				            bytes_to_write);
				m_decompressed_bytes_read += bytes_to_write;
				out->bytes_written += bytes_to_write;
			} else {
				const std::size_t prev_out_bytes_written = out->bytes_written;
				const std::size_t old_out_size = out->data.size();
				const std::size_t new_out_size = out->bytes_written + bytes_to_write;

				if (new_out_size < old_out_size)
					out->data.resize(new_out_size);

				if (!m_decompressor->Decompress(in, out, in_bytes_read))
					return false;

				out->data.resize(old_out_size);

				bytes_to_write = out->bytes_written - prev_out_bytes_written;

				if (!IncrementBytesRead(bytes_to_write))
					return false;

				if (bytes_to_write == 0)
					return true;
			}

			m_data_offset += bytes_to_write;
			m_size -= static_cast<u32>(bytes_to_write);
		}

		// If out is full but not all data has been read from in, give the decompressor a chance to
		// read from in anyway. This is needed when zstd has read everything except the checksum.
		if (out->data.size() == out->bytes_written && in.bytes_written != *in_bytes_read) {
			if (!m_decompressor->Decompress(in, out, in_bytes_read))
				return false;
		}

		return true;
	}

	bool Done() const override {
		return m_size == 0 && m_rvz_packed_size == m_bytes_read &&
		       m_decompressed.bytes_written == m_decompressed_bytes_read && m_decompressor->Done();
	}

private:
	bool IncrementBytesRead(std::size_t x) {
		m_bytes_read += x;
		return m_bytes_read <= m_rvz_packed_size;
	}

	// -1: the bytes are available. 0: error. 1: need more input.
	int ReadToDecompressed(const DecompressionBuffer& in, std::size_t* in_bytes_read,
	                       std::size_t decompressed_bytes_read, std::size_t bytes_to_read) {
		if (m_decompressed.data.size() < decompressed_bytes_read + bytes_to_read)
			m_decompressed.data.resize(decompressed_bytes_read + bytes_to_read);

		if (m_decompressed.bytes_written < decompressed_bytes_read + bytes_to_read) {
			const std::size_t prev_bytes_written = m_decompressed.bytes_written;

			if (!m_decompressor->Decompress(in, &m_decompressed, in_bytes_read))
				return 0;

			if (!IncrementBytesRead(m_decompressed.bytes_written - prev_bytes_written))
				return 0;

			if (m_decompressed.bytes_written < decompressed_bytes_read + bytes_to_read)
				return 1;
		}

		return -1;
	}

	std::unique_ptr<Decompressor> m_decompressor;
	DecompressionBuffer m_decompressed;
	std::size_t m_decompressed_bytes_read = 0;
	std::size_t m_bytes_read;
	u64 m_data_offset;
	u32 m_rvz_packed_size;

	u32 m_size = 0;
	bool m_junk = false;
	LaggedFibonacciGenerator m_lfg;
};

// ---- Compressors ------------------------------------------------------------------------------

class PurgeCompressor final : public Compressor {
public:
	bool Start(u64) override {
		m_buffer.clear();
		m_bytes_written = 0;
		m_sha1.Reset();
		return true;
	}

	bool AddPrecedingDataOnlyForPurgeHashing(const u8* data, std::size_t size) override {
		m_sha1.Update(data, size);
		return true;
	}

	bool Compress(const u8* data, std::size_t size) override {
		// Calling this twice isn't supported (and isn't needed)
		if (m_bytes_written != 0)
			return false;

		m_buffer.resize(size + sizeof(PurgeSegment) + SHA1::DIGEST_LEN);

		std::size_t bytes_read = 0;
		while (true) {
			const u8* first_non_zero = std::find_if(data + bytes_read, data + size, [](u8 x) { return x != 0; });

			const u32 non_zero_data_start = static_cast<u32>(first_non_zero - data);
			if (non_zero_data_start == size)
				break;

			std::size_t non_zero_data_end = non_zero_data_start;
			std::size_t sequence_length = 0;
			for (std::size_t i = non_zero_data_start; i < size; ++i) {
				if (data[i] == 0) {
					++sequence_length;
				} else {
					sequence_length = 0;
					non_zero_data_end = i + 1;
				}

				// To avoid wasting space, only count runs of zeroes that are of a certain length
				// (unless there is nothing after the run of zeroes, then we might as well always count it)
				if (sequence_length > sizeof(PurgeSegment))
					break;
			}

			const u32 non_zero_data_length = static_cast<u32>(non_zero_data_end - non_zero_data_start);

			const PurgeSegment segment{swap32(non_zero_data_start), swap32(non_zero_data_length)};
			std::memcpy(m_buffer.data() + m_bytes_written, &segment, sizeof(segment));
			m_bytes_written += sizeof(segment);

			std::memcpy(m_buffer.data() + m_bytes_written, data + non_zero_data_start, non_zero_data_length);
			m_bytes_written += non_zero_data_length;

			bytes_read = non_zero_data_end;
		}

		return true;
	}

	bool End() override {
		if (m_buffer.size() < m_bytes_written + SHA1::DIGEST_LEN)
			m_buffer.resize(m_bytes_written + SHA1::DIGEST_LEN);  // Compress() was never called
		m_sha1.Update(m_buffer.data(), m_bytes_written);
		const SHA1::Digest digest = m_sha1.Finish();
		std::memcpy(m_buffer.data() + m_bytes_written, digest.data(), digest.size());
		m_bytes_written += digest.size();
		return true;
	}

	const u8* GetData() const override { return m_buffer.data(); }
	std::size_t GetSize() const override { return m_bytes_written; }

private:
	std::vector<u8> m_buffer;
	std::size_t m_bytes_written = 0;
	SHA1::Context m_sha1;
};

class Bzip2Compressor final : public Compressor {
public:
	explicit Bzip2Compressor(int level) : m_level(level) {}
	~Bzip2Compressor() override {
		if (m_started)
			BZ2_bzCompressEnd(&m_stream);
	}

	bool Start(u64) override {
		if (m_started)
			BZ2_bzCompressEnd(&m_stream);
		m_stream = {};
		m_buffer.clear();
		m_stream.next_out = reinterpret_cast<char*>(m_buffer.data());
		m_started = BZ2_bzCompressInit(&m_stream, m_level, 0, 0) == BZ_OK;
		return m_started;
	}

	bool Compress(const u8* data, std::size_t size) override {
		m_stream.next_in = reinterpret_cast<char*>(const_cast<u8*>(data));
		m_stream.avail_in = static_cast<unsigned>(size);

		ExpandBuffer(size);

		while (m_stream.avail_in != 0) {
			if (m_stream.avail_out == 0)
				ExpandBuffer(0x100);
			if (BZ2_bzCompress(&m_stream, BZ_RUN) != BZ_RUN_OK)
				return false;
		}
		return true;
	}

	bool End() override {
		bool success = true;
		while (true) {
			if (m_stream.avail_out == 0)
				ExpandBuffer(0x100);
			const int result = BZ2_bzCompress(&m_stream, BZ_FINISH);
			if (result != BZ_FINISH_OK && result != BZ_STREAM_END)
				success = false;
			if (result != BZ_FINISH_OK)
				break;
		}
		m_size = GetCurrentSize();
		if (BZ2_bzCompressEnd(&m_stream) != BZ_OK)
			success = false;
		m_started = false;
		return success;
	}

	const u8* GetData() const override { return m_buffer.data(); }
	std::size_t GetSize() const override { return m_size; }

private:
	std::size_t GetCurrentSize() const {
		return static_cast<std::size_t>(reinterpret_cast<const u8*>(m_stream.next_out) - m_buffer.data());
	}

	void ExpandBuffer(std::size_t bytes_to_add) {
		const std::size_t bytes_written = GetCurrentSize();
		m_buffer.resize(m_buffer.size() + bytes_to_add);
		m_stream.next_out = reinterpret_cast<char*>(m_buffer.data()) + bytes_written;
		m_stream.avail_out = static_cast<unsigned>(m_buffer.size() - bytes_written);
	}

	bz_stream m_stream = {};
	std::vector<u8> m_buffer;
	std::size_t m_size = 0;
	int m_level;
	bool m_started = false;
};

// The LZMA SDK encoders work on whole buffers, so the input is gathered until End().
class LZMACompressor final : public Compressor {
public:
	LZMACompressor(bool lzma2, int level, u8* compressor_data_out, u8* compressor_data_size_out)
	    : m_lzma2(lzma2), m_level(std::clamp(level, 0, 9)), m_dict_size(LZMADictionarySize(level)) {
		if (!lzma2) {
			if (compressor_data_size_out)
				*compressor_data_size_out = 5;
			if (compressor_data_out) {
				// lc = 3, lp = 0, pb = 2, like liblzma's presets
				compressor_data_out[0] = static_cast<u8>((2 * 5 + 0) * 9 + 3);
				for (int i = 0; i < 4; ++i)
					compressor_data_out[1 + i] = static_cast<u8>(m_dict_size >> (8 * i));
			}
		} else {
			if (compressor_data_size_out)
				*compressor_data_size_out = 1;
			if (compressor_data_out) {
				u8 encoded_dict_size = 0;
				while (encoded_dict_size < 40 && m_dict_size > LZMA2DictionarySize(encoded_dict_size))
					++encoded_dict_size;
				compressor_data_out[0] = encoded_dict_size;
			}
		}
	}

	bool Start(u64 size) override {
		m_input.clear();
		m_input.reserve(static_cast<std::size_t>(size));
		m_output.clear();
		return true;
	}

	bool Compress(const u8* data, std::size_t size) override {
		m_input.insert(m_input.end(), data, data + size);
		return true;
	}

	bool End() override {
		const std::size_t in_size = m_input.size();
		CLzmaEncProps lzma;
		LzmaEncProps_Init(&lzma);
		lzma.level = m_level;
		lzma.dictSize = m_dict_size;
		lzma.lc = 3;
		lzma.lp = 0;
		lzma.pb = 2;
		lzma.numThreads = 1;
		lzma.reduceSize = in_size;

		if (!m_lzma2) {
			lzma.writeEndMark = 1;
			std::size_t out_size = in_size + in_size / 3 + 128;
			m_output.resize(out_size);
			u8 props[LZMA_PROPS_SIZE];
			SizeT props_size = LZMA_PROPS_SIZE;
			SizeT dest_len = out_size;
			const SRes res = LzmaEncode(m_output.data(), &dest_len, m_input.data(), in_size, &lzma, props, &props_size,
			                            1, nullptr, &g_alloc, &g_alloc);
			if (res != SZ_OK)
				return false;
			m_output.resize(dest_len);
			return true;
		}

		CLzma2EncHandle enc = Lzma2Enc_Create(&g_alloc, &g_alloc);
		if (!enc)
			return false;
		CLzma2EncProps props;
		Lzma2EncProps_Init(&props);
		props.lzmaProps = lzma;
		props.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
		props.numBlockThreads_Max = 1;
		props.numTotalThreads = 1;
		SRes res = Lzma2Enc_SetProps(enc, &props);
		Lzma2Enc_SetDataSize(enc, in_size);
		std::size_t out_size = in_size + in_size / 32 + 1024;
		m_output.resize(out_size);
		if (res == SZ_OK)
			res = Lzma2Enc_Encode2(enc, nullptr, m_output.data(), &out_size, nullptr, m_input.data(), in_size, nullptr);
		Lzma2Enc_Destroy(enc);
		if (res != SZ_OK)
			return false;
		m_output.resize(out_size);
		return true;
	}

	const u8* GetData() const override { return m_output.data(); }
	std::size_t GetSize() const override { return m_output.size(); }

private:
	bool m_lzma2;
	int m_level;
	u32 m_dict_size;
	std::vector<u8> m_input, m_output;
};

class ZstdCompressor final : public Compressor {
public:
	explicit ZstdCompressor(int level) : m_stream(ZSTD_createCStream()) {
		if (m_stream && (ZSTD_isError(ZSTD_CCtx_setParameter(m_stream, ZSTD_c_compressionLevel, level)) ||
		                 ZSTD_isError(ZSTD_CCtx_setParameter(m_stream, ZSTD_c_contentSizeFlag, 0)))) {
			ZSTD_freeCStream(m_stream);
			m_stream = nullptr;
		}
	}
	~ZstdCompressor() override { ZSTD_freeCStream(m_stream); }

	bool Start(u64 size) override {
		if (!m_stream)
			return false;
		m_buffer.clear();
		m_out_buffer = {};
		if (ZSTD_isError(ZSTD_CCtx_reset(m_stream, ZSTD_reset_session_only)))
			return false;
		return !ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(m_stream, size));
	}

	bool Compress(const u8* data, std::size_t size) override {
		ZSTD_inBuffer in_buffer{data, size, 0};
		ExpandBuffer(size);
		while (in_buffer.size != in_buffer.pos) {
			if (m_out_buffer.size == m_out_buffer.pos)
				ExpandBuffer(0x100);
			if (ZSTD_isError(ZSTD_compressStream(m_stream, &m_out_buffer, &in_buffer)))
				return false;
		}
		return true;
	}

	bool End() override {
		while (true) {
			if (m_out_buffer.size == m_out_buffer.pos)
				ExpandBuffer(0x100);
			const std::size_t result = ZSTD_endStream(m_stream, &m_out_buffer);
			if (ZSTD_isError(result))
				return false;
			if (result == 0)
				return true;
		}
	}

	const u8* GetData() const override { return m_buffer.data(); }
	std::size_t GetSize() const override { return m_out_buffer.pos; }

private:
	void ExpandBuffer(std::size_t bytes_to_add) {
		m_buffer.resize(m_buffer.size() + bytes_to_add);
		m_out_buffer.dst = m_buffer.data();
		m_out_buffer.size = m_buffer.size();
	}

	ZSTD_CStream* m_stream;
	ZSTD_outBuffer m_out_buffer{};
	std::vector<u8> m_buffer;
};

}  // namespace

std::pair<int, int> GetAllowedCompressionLevels(CompressionType type) {
	switch (type) {
	case CompressionType::Bzip2:
	case CompressionType::LZMA:
	case CompressionType::LZMA2:
		return {1, 9};
	case CompressionType::Zstd:
		// Level 0 means "default level" (3) to zstd.
		return {ZSTD_minCLevel(), ZSTD_maxCLevel()};
	default:
		return {0, -1};
	}
}

std::unique_ptr<Decompressor> CreateDecompressor(CompressionType type, const u8* compressor_data,
                                                 std::size_t compressor_data_size, u64 max_output, u64 purge_size) {
	switch (type) {
	case CompressionType::None:
		return std::make_unique<NoneDecompressor>();
	case CompressionType::Purge:
		return std::make_unique<PurgeDecompressor>(purge_size);
	case CompressionType::Bzip2:
		return std::make_unique<Bzip2Decompressor>();
	case CompressionType::LZMA:
	case CompressionType::LZMA2:
		return std::make_unique<LZMADecompressor>(type == CompressionType::LZMA2, compressor_data,
		                                          compressor_data_size, max_output);
	case CompressionType::Zstd:
		return std::make_unique<ZstdDecompressor>();
	}
	return nullptr;
}

std::unique_ptr<Decompressor> CreateRVZPackDecompressor(std::unique_ptr<Decompressor> decompressor,
                                                        DecompressionBuffer decompressed, u64 data_offset,
                                                        u32 rvz_packed_size) {
	return std::make_unique<RVZPackDecompressor>(std::move(decompressor), std::move(decompressed), data_offset,
	                                             rvz_packed_size);
}

std::unique_ptr<Compressor> CreateCompressor(CompressionType type, int level, u8* compressor_data_out,
                                             u8* compressor_data_size_out) {
	switch (type) {
	case CompressionType::None:
		return nullptr;
	case CompressionType::Purge:
		return std::make_unique<PurgeCompressor>();
	case CompressionType::Bzip2:
		return std::make_unique<Bzip2Compressor>(level);
	case CompressionType::LZMA:
	case CompressionType::LZMA2:
		return std::make_unique<LZMACompressor>(type == CompressionType::LZMA2, level, compressor_data_out,
		                                        compressor_data_size_out);
	case CompressionType::Zstd:
		return std::make_unique<ZstdCompressor>(level);
	}
	return nullptr;
}

}  // namespace irvz
