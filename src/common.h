// iRVZ - WIA/RVZ disc image library
// Copyright 2018-2020 Dolphin Emulator Project, 2026 iRVZ contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small helpers shared by every file: fixed-width types, big-endian conversion and alignment.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace irvz {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

// WIA/RVZ and Wii discs store everything big-endian. These convert between big-endian and the host
// order (a byte swap on little-endian hosts, nothing on big-endian ones), like Dolphin's
// Common::swap on the little-endian machines it supports.
inline u16 swap16(u16 x) {
	if constexpr (std::endian::native == std::endian::big)
		return x;
	return static_cast<u16>((x >> 8) | (x << 8));
}
inline u32 swap32(u32 x) {
	if constexpr (std::endian::native == std::endian::big)
		return x;
	return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
}
inline u64 swap64(u64 x) {
	return (static_cast<u64>(swap32(static_cast<u32>(x))) << 32) | swap32(static_cast<u32>(x >> 32));
}
inline u16 swap16(const u8* p) {
	u16 x;
	std::memcpy(&x, p, sizeof(x));
	return swap16(x);
}
inline u32 swap32(const u8* p) {
	u32 x;
	std::memcpy(&x, p, sizeof(x));
	return swap32(x);
}

template <typename T>
constexpr T AlignUp(T value, std::size_t size) {
	return static_cast<T>(value + (size - value % size) % size);
}
template <typename T>
constexpr T AlignDown(T value, std::size_t size) {
	return static_cast<T>(value - value % size);
}

}  // namespace irvz
