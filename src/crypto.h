// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-1 and AES-128-CBC: the only cryptography WIA/RVZ needs (header hashes, Purge checksums,
// Wii partition hashing and encryption).
#pragma once

#include <array>
#include <cstddef>

#include "common.h"

namespace irvz {

namespace SHA1 {
constexpr std::size_t DIGEST_LEN = 20;
using Digest = std::array<u8, DIGEST_LEN>;

class Context {
public:
	Context() { Reset(); }
	void Reset();
	void Update(const void* data, std::size_t size);
	Digest Finish();

private:
	void Block(const u8* p);
	u32 h_[5];
	u64 length_;
	u8 buffer_[64];
	std::size_t used_;
};

Digest CalculateDigest(const void* data, std::size_t size);
}  // namespace SHA1

// AES-128 in CBC mode (Wii partitions). Encrypt and decrypt contexts are separate because the key
// schedules differ.
class AES128 {
public:
	enum class Mode { Encrypt, Decrypt };
	AES128(const u8 key[16], Mode mode);
	// CBC over size bytes (a multiple of 16). iv is not modified; in and out may be the same.
	void Crypt(const u8* iv, const u8* in, u8* out, std::size_t size) const;
	void CryptIvZero(const u8* in, u8* out, std::size_t size) const;

private:
	void EncryptBlock(const u8* in, u8* out) const;
	void DecryptBlock(const u8* in, u8* out) const;
	u32 rk_[44];
	Mode mode_;
};

}  // namespace irvz
