# Changelog

## 1.0.0 — 2026-10-17

First release.

- WIA and RVZ reading and writing, ported from Dolphin's `DiscIO/WIABlob.cpp`: every compression
  method (none, Purge, bzip2, LZMA, LZMA2, Zstandard) and chunk size Dolphin supports, Wii
  partition hashing/encryption with hash exceptions, RVZ junk-data packing, and group reuse.
- Conversion from plain ISO/GCM, and between WIA and RVZ.
- C API (`include/irvz/irvz.h`) with file, memory and callback I/O: images can be compressed and
  extracted entirely in RAM. Random-access reads and multithreaded extraction.
- `irvz` command line tool: `info`, `compress`, `extract` (with `--ram`).
- Bundled zstd, LZMA SDK 26.02 and bzip2 1.0.8; host projects can supply their own targets.
- Round-trip self-test on synthetic GameCube and Wii discs.
- Verified in Dolphin and with a real Wii game: images play correctly and extract to the original
  ISO unchanged. Output is slightly smaller than Dolphin's for the same settings.
