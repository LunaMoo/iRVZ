// iRVZ - WIA/RVZ disc image library
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Plain C++ SHA-1 and table-based AES-128, with an AES-NI path picked at run time on x86.

#include "crypto.h"

#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define IRVZ_X86 1
#include <wmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace irvz {

// ---- SHA-1 ------------------------------------------------------------------------------------

namespace SHA1 {

static inline u32 Rol(u32 x, int n) {
	return (x << n) | (x >> (32 - n));
}

void Context::Reset() {
	h_[0] = 0x67452301;
	h_[1] = 0xEFCDAB89;
	h_[2] = 0x98BADCFE;
	h_[3] = 0x10325476;
	h_[4] = 0xC3D2E1F0;
	length_ = 0;
	used_ = 0;
}

void Context::Block(const u8* p) {
	u32 w[80];
	for (int i = 0; i < 16; ++i)
		w[i] = (u32(p[4 * i]) << 24) | (u32(p[4 * i + 1]) << 16) | (u32(p[4 * i + 2]) << 8) | p[4 * i + 3];
	for (int i = 16; i < 80; ++i)
		w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
	for (int i = 0; i < 80; ++i) {
		u32 f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		const u32 t = Rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = Rol(b, 30);
		b = a;
		a = t;
	}
	h_[0] += a;
	h_[1] += b;
	h_[2] += c;
	h_[3] += d;
	h_[4] += e;
}

void Context::Update(const void* data, std::size_t size) {
	if (size == 0)
		return;
	const u8* p = static_cast<const u8*>(data);
	length_ += size;
	if (used_) {
		const std::size_t n = size < 64 - used_ ? size : 64 - used_;
		std::memcpy(buffer_ + used_, p, n);
		used_ += n;
		p += n;
		size -= n;
		if (used_ < 64)
			return;
		Block(buffer_);
		used_ = 0;
	}
	for (; size >= 64; size -= 64, p += 64)
		Block(p);
	std::memcpy(buffer_, p, size);
	used_ = size;
}

Digest Context::Finish() {
	const u64 bits = length_ * 8;
	const u8 pad = 0x80;
	Update(&pad, 1);
	const u8 zero = 0;
	while (used_ != 56)
		Update(&zero, 1);
	u8 len[8];
	for (int i = 0; i < 8; ++i)
		len[i] = static_cast<u8>(bits >> (56 - 8 * i));
	Update(len, 8);
	Digest out;
	for (int i = 0; i < 5; ++i)
		for (int j = 0; j < 4; ++j)
			out[4 * i + j] = static_cast<u8>(h_[i] >> (24 - 8 * j));
	Reset();
	return out;
}

Digest CalculateDigest(const void* data, std::size_t size) {
	Context ctx;
	ctx.Update(data, size);
	return ctx.Finish();
}

}  // namespace SHA1

// ---- AES-128 ----------------------------------------------------------------------------------

namespace {

struct Tables {
	u8 sbox[256], inv[256];
	u32 te[256], td[256];  // first column tables; the others are byte rotations of these

	Tables() {
		auto rotl8 = [](u8 x, int n) { return static_cast<u8>((x << n) | (x >> (8 - n))); };
		u8 p = 1, q = 1;
		do {
			p = static_cast<u8>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
			q ^= static_cast<u8>(q << 1);
			q ^= static_cast<u8>(q << 2);
			q ^= static_cast<u8>(q << 4);
			if (q & 0x80)
				q ^= 0x09;
			sbox[p] = static_cast<u8>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
		} while (p != 1);
		sbox[0] = 0x63;
		for (int i = 0; i < 256; ++i)
			inv[sbox[i]] = static_cast<u8>(i);

		auto mul = [](u8 a, u8 b) {
			u8 r = 0;
			while (b) {
				if (b & 1)
					r ^= a;
				a = static_cast<u8>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
				b >>= 1;
			}
			return r;
		};
		for (int i = 0; i < 256; ++i) {
			const u8 s = sbox[i], t = inv[i];
			te[i] = (u32(mul(s, 2)) << 24) | (u32(s) << 16) | (u32(s) << 8) | mul(s, 3);
			td[i] = (u32(mul(t, 14)) << 24) | (u32(mul(t, 9)) << 16) | (u32(mul(t, 13)) << 8) | mul(t, 11);
		}
	}
};

const Tables& T() {
	static const Tables tables;
	return tables;
}

inline u32 Ror(u32 x, int n) {
	return (x >> n) | (x << (32 - n));
}
inline u32 Load(const u8* p) {
	return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
inline void Store(u8* p, u32 x) {
	p[0] = static_cast<u8>(x >> 24);
	p[1] = static_cast<u8>(x >> 16);
	p[2] = static_cast<u8>(x >> 8);
	p[3] = static_cast<u8>(x);
}

#ifdef IRVZ_X86
bool HasAesNi() {
	static const bool has = [] {
#if defined(_MSC_VER)
		int regs[4];
		__cpuid(regs, 1);
		return (regs[2] & (1 << 25)) != 0;
#else
		unsigned a, b, c, d;
		if (!__get_cpuid(1, &a, &b, &c, &d))
			return false;
		return (c & (1u << 25)) != 0;
#endif
	}();
	return has;
}

#if defined(__GNUC__) || defined(__clang__)
#define IRVZ_AES_TARGET __attribute__((target("aes,sse2")))
#else
#define IRVZ_AES_TARGET
#endif

IRVZ_AES_TARGET void CbcNi(const u32* rk_words, bool encrypt, const u8* iv, const u8* in, u8* out, std::size_t size) {
	__m128i rk[11];
	for (int r = 0; r < 11; ++r) {
		u8 b[16];
		for (int i = 0; i < 4; ++i)
			Store(b + 4 * i, rk_words[4 * r + i]);
		rk[r] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
	}
	__m128i chain = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
	for (std::size_t off = 0; off < size; off += 16) {
		__m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + off));
		if (encrypt) {
			__m128i x = _mm_xor_si128(_mm_xor_si128(block, chain), rk[0]);
			for (int r = 1; r < 10; ++r)
				x = _mm_aesenc_si128(x, rk[r]);
			chain = _mm_aesenclast_si128(x, rk[10]);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(out + off), chain);
		} else {
			__m128i x = _mm_xor_si128(block, rk[0]);
			for (int r = 1; r < 10; ++r)
				x = _mm_aesdec_si128(x, rk[r]);
			x = _mm_xor_si128(_mm_aesdeclast_si128(x, rk[10]), chain);
			chain = block;
			_mm_storeu_si128(reinterpret_cast<__m128i*>(out + off), x);
		}
	}
}
#endif

}  // namespace

AES128::AES128(const u8 key[16], Mode mode) : mode_(mode) {
	const Tables& t = T();
	u32 ek[44];
	for (int i = 0; i < 4; ++i)
		ek[i] = Load(key + 4 * i);
	u8 rcon = 1;
	for (int i = 4; i < 44; ++i) {
		u32 x = ek[i - 1];
		if (i % 4 == 0) {
			x = (u32(t.sbox[(x >> 16) & 0xFF]) << 24) | (u32(t.sbox[(x >> 8) & 0xFF]) << 16) |
			    (u32(t.sbox[x & 0xFF]) << 8) | t.sbox[x >> 24];
			x ^= u32(rcon) << 24;
			rcon = static_cast<u8>((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
		}
		ek[i] = ek[i - 4] ^ x;
	}
	if (mode == Mode::Encrypt) {
		std::memcpy(rk_, ek, sizeof(rk_));
		return;
	}
	// Equivalent inverse cipher: reversed round keys, InvMixColumns on the middle ones.
	for (int r = 0; r <= 10; ++r) {
		for (int i = 0; i < 4; ++i) {
			u32 w = ek[4 * (10 - r) + i];
			if (r != 0 && r != 10) {
				w = t.td[t.sbox[w >> 24]] ^ Ror(t.td[t.sbox[(w >> 16) & 0xFF]], 8) ^
				    Ror(t.td[t.sbox[(w >> 8) & 0xFF]], 16) ^ Ror(t.td[t.sbox[w & 0xFF]], 24);
			}
			rk_[4 * r + i] = w;
		}
	}
}

void AES128::EncryptBlock(const u8* in, u8* out) const {
	const Tables& t = T();
	const u32* rk = rk_;
	u32 s0 = Load(in) ^ rk[0], s1 = Load(in + 4) ^ rk[1], s2 = Load(in + 8) ^ rk[2], s3 = Load(in + 12) ^ rk[3];
	for (int r = 1; r < 10; ++r) {
		rk += 4;
		const u32 t0 = t.te[s0 >> 24] ^ Ror(t.te[(s1 >> 16) & 0xFF], 8) ^ Ror(t.te[(s2 >> 8) & 0xFF], 16) ^ Ror(t.te[s3 & 0xFF], 24) ^ rk[0];
		const u32 t1 = t.te[s1 >> 24] ^ Ror(t.te[(s2 >> 16) & 0xFF], 8) ^ Ror(t.te[(s3 >> 8) & 0xFF], 16) ^ Ror(t.te[s0 & 0xFF], 24) ^ rk[1];
		const u32 t2 = t.te[s2 >> 24] ^ Ror(t.te[(s3 >> 16) & 0xFF], 8) ^ Ror(t.te[(s0 >> 8) & 0xFF], 16) ^ Ror(t.te[s1 & 0xFF], 24) ^ rk[2];
		const u32 t3 = t.te[s3 >> 24] ^ Ror(t.te[(s0 >> 16) & 0xFF], 8) ^ Ror(t.te[(s1 >> 8) & 0xFF], 16) ^ Ror(t.te[s2 & 0xFF], 24) ^ rk[3];
		s0 = t0, s1 = t1, s2 = t2, s3 = t3;
	}
	rk += 4;
	auto last = [&](u32 a, u32 b, u32 c, u32 d, u32 k) {
		return ((u32(t.sbox[a >> 24]) << 24) | (u32(t.sbox[(b >> 16) & 0xFF]) << 16) |
		        (u32(t.sbox[(c >> 8) & 0xFF]) << 8) | t.sbox[d & 0xFF]) ^ k;
	};
	Store(out, last(s0, s1, s2, s3, rk[0]));
	Store(out + 4, last(s1, s2, s3, s0, rk[1]));
	Store(out + 8, last(s2, s3, s0, s1, rk[2]));
	Store(out + 12, last(s3, s0, s1, s2, rk[3]));
}

void AES128::DecryptBlock(const u8* in, u8* out) const {
	const Tables& t = T();
	const u32* rk = rk_;
	u32 s0 = Load(in) ^ rk[0], s1 = Load(in + 4) ^ rk[1], s2 = Load(in + 8) ^ rk[2], s3 = Load(in + 12) ^ rk[3];
	for (int r = 1; r < 10; ++r) {
		rk += 4;
		const u32 t0 = t.td[s0 >> 24] ^ Ror(t.td[(s3 >> 16) & 0xFF], 8) ^ Ror(t.td[(s2 >> 8) & 0xFF], 16) ^ Ror(t.td[s1 & 0xFF], 24) ^ rk[0];
		const u32 t1 = t.td[s1 >> 24] ^ Ror(t.td[(s0 >> 16) & 0xFF], 8) ^ Ror(t.td[(s3 >> 8) & 0xFF], 16) ^ Ror(t.td[s2 & 0xFF], 24) ^ rk[1];
		const u32 t2 = t.td[s2 >> 24] ^ Ror(t.td[(s1 >> 16) & 0xFF], 8) ^ Ror(t.td[(s0 >> 8) & 0xFF], 16) ^ Ror(t.td[s3 & 0xFF], 24) ^ rk[2];
		const u32 t3 = t.td[s3 >> 24] ^ Ror(t.td[(s2 >> 16) & 0xFF], 8) ^ Ror(t.td[(s1 >> 8) & 0xFF], 16) ^ Ror(t.td[s0 & 0xFF], 24) ^ rk[3];
		s0 = t0, s1 = t1, s2 = t2, s3 = t3;
	}
	rk += 4;
	auto last = [&](u32 a, u32 b, u32 c, u32 d, u32 k) {
		return ((u32(t.inv[a >> 24]) << 24) | (u32(t.inv[(b >> 16) & 0xFF]) << 16) |
		        (u32(t.inv[(c >> 8) & 0xFF]) << 8) | t.inv[d & 0xFF]) ^ k;
	};
	Store(out, last(s0, s3, s2, s1, rk[0]));
	Store(out + 4, last(s1, s0, s3, s2, rk[1]));
	Store(out + 8, last(s2, s1, s0, s3, rk[2]));
	Store(out + 12, last(s3, s2, s1, s0, rk[3]));
}

void AES128::Crypt(const u8* iv, const u8* in, u8* out, std::size_t size) const {
#ifdef IRVZ_X86
	if (HasAesNi()) {
		CbcNi(rk_, mode_ == Mode::Encrypt, iv, in, out, size);
		return;
	}
#endif
	u8 chain[16], next[16];
	std::memcpy(chain, iv, 16);
	for (std::size_t off = 0; off < size; off += 16) {
		if (mode_ == Mode::Encrypt) {
			u8 x[16];
			for (int i = 0; i < 16; ++i)
				x[i] = in[off + i] ^ chain[i];
			EncryptBlock(x, out + off);
			std::memcpy(chain, out + off, 16);
		} else {
			std::memcpy(next, in + off, 16);
			DecryptBlock(in + off, out + off);
			for (int i = 0; i < 16; ++i)
				out[off + i] ^= chain[i];
			std::memcpy(chain, next, 16);
		}
	}
}

void AES128::CryptIvZero(const u8* in, u8* out, std::size_t size) const {
	static const u8 zero[16] = {};
	Crypt(zero, in, out, size);
}

}  // namespace irvz
