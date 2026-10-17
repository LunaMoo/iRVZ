# iRVZ

A small, standalone library and command line tool for the **WIA** and **RVZ** compressed
GameCube / Wii disc image formats — Dolphin's `Source/Core/DiscIO/WIABlob.cpp` without the rest of
Dolphin. Images can be read from and written to files, memory, or your own callbacks, so a host
program can repack between formats entirely in RAM.

Made by Claude AI based on LunaMoo idea. Licensed under GPL-3.0 (the WIA/RVZ code is Dolphin's,
GPL-2.0-or-later).

## What's supported

Everything WIABlob.cpp supports:

| | WIA | RVZ |
|---|---|---|
| Compression | none, Purge, bzip2, LZMA, LZMA2 | none, bzip2, LZMA, LZMA2, Zstandard |
| Chunk sizes | multiples of 2 MiB | powers of two from 32 KiB, or multiples of 2 MiB |
| Read / write | yes / yes | yes / yes |

Wii partitions are stored decrypted with hashes stripped (with hash exceptions for any that don't
match), and rebuilt bit-for-bit on reading; RVZ also stores Nintendo's padding ("junk") data as
seeds. Input for compression can be a plain ISO/GCM or another WIA/RVZ (converts between them).

Compared with Dolphin, the only intentional differences are:
- LZMA and LZMA2 use the LZMA SDK (26.02, the same copy CHDmodern ships) instead of liblzma. The
  streams are the same format, but the compressed bytes differ from Dolphin's output.
- Whole-image extraction is multithreaded; dictionaries are capped at the chunk size when reading.
- A buffer overrun in Dolphin's WIA writer (chunks over 2 MiB ending in a partial Wii group) is fixed.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build        # round-trip self-test
```

Visual Studio opens the folder directly (CMake). Produces `iRVZ` (static library; `-DIRVZ_SHARED=ON`
for a shared one) and the `irvz` tool. zstd, the LZMA SDK and bzip2 are bundled in `third_party/`.

## Command line

```
irvz info game.rvz
irvz compress game.iso game.rvz                       # RVZ, zstd 5, 128 KiB chunks (Dolphin's defaults)
irvz compress -f wia -c lzma2 -l 9 game.iso game.wia
irvz compress -c lzma -b 2M game.wia game.rvz         # WIA -> RVZ
irvz extract -t 8 game.rvz game.iso
```
`--ram` loads the input into memory and builds the output in memory (exercises the RAM API).

## Library

Plain C, `include/irvz/irvz.h`:

```c
#include <irvz/irvz.h>

// Random access to the decompressed image
irvz_file *f;
irvz_open("game.rvz", &f);            // or irvz_open_memory(ptr, size, &f) / irvz_open_reader(...)
irvz_read(f, offset, buf, len);
irvz_close(f);

// RAM to RAM
irvz_buffer rvz = {0}, iso = {0};
irvz_compress_options o;
irvz_compress_options_init(&o, IRVZ_FORMAT_RVZ);
irvz_compress_memory(iso_data, iso_size, &rvz, &o, progress, user);
irvz_extract_memory(rvz.data, rvz.size, &iso, 0 /* threads: all cores */, progress, user);
irvz_buffer_free(&rvz);
irvz_buffer_free(&iso);
```

`irvz_compress` / `irvz_extract` take `irvz_reader` / `irvz_writer` callbacks
(`read(offset, buf, len)`, `begin(size)`, `write(offset, buf, len)`, `finish(size)`), the same shape
as CHDmodern's and iPBP's, so ISOPackager's `ImageReader` / `ImageWriter` (e.g. `MemoryImage`) plug
straight in. Progress callbacks run on the calling thread and cancel by returning nonzero; extraction
writes from several threads at once.

### As a submodule (ISOPackager)

```cmake
set(IRVZ_SHARED OFF CACHE BOOL "" FORCE)
set(IRVZ_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(IRVZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IRVZ_ZSTD_TARGET chdm_zstd CACHE STRING "" FORCE)   # optional: reuse CHDmodern's zstd
add_subdirectory(external/iRVZ ${CMAKE_BINARY_DIR}/ext/iRVZ EXCLUDE_FROM_ALL)
target_link_libraries(isopk_core PUBLIC irvz)
```
`IRVZ_LZMA_TARGET` and `IRVZ_BZIP2_TARGET` likewise accept existing targets. An LZMA target must
include `Lzma2Dec.c` / `Lzma2Enc.c` (from `third_party/lzma/C`, identical to CHDmodern's version), be
built with `Z7_ST` and expose its headers as `lzma/C/...`.

## Layout

```
include/irvz/irvz.h   C API
src/wia.*             WIA/RVZ reader and writer (WIABlob.cpp, WiiEncryptionCache, MultithreadedCompressor)
src/compression.*     chunk codecs and RVZ packing (WIACompression.cpp)
src/lfg.*             junk data generator (LaggedFibonacciGenerator.cpp)
src/disc.*            Wii partitions, tickets, hashing, FST (from Volume*.cpp, FileSystemGCWii.cpp)
src/crypto.*          SHA-1, AES-128-CBC (AES-NI when available)
src/io.*, src/irvz.cpp  files / memory / callbacks, and the C API
tools/irvz_cli.cpp    the irvz tool
tests/selftest.cpp    round-trip tests on synthetic GameCube and Wii discs
```
