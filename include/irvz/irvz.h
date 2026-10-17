/*
 * iRVZ - WIA/RVZ disc image library
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads and writes the WIA and RVZ compressed GameCube / Wii disc image formats (as used by
 * Dolphin and wit), from and to files or memory. Plain C interface; link the static or shared
 * library named iRVZ.
 *
 * Threads: an irvz_file handle must be used by one thread at a time; separate handles (and the
 * whole-image functions) can run in parallel.
 */
#ifndef IRVZ_H
#define IRVZ_H

#include <stddef.h>
#include <stdint.h>

#define IRVZ_VERSION_MAJOR 1
#define IRVZ_VERSION_MINOR 0
#define IRVZ_VERSION_PATCH 0

#if defined(IRVZ_SHARED)
#if defined(_WIN32)
#if defined(IRVZ_BUILDING)
#define IRVZ_API __declspec(dllexport)
#else
#define IRVZ_API __declspec(dllimport)
#endif
#else
#define IRVZ_API __attribute__((visibility("default")))
#endif
#else
#define IRVZ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. */
enum {
	IRVZ_OK = 0,
	IRVZ_ERR_CANCELLED = 1, /* the progress callback asked to stop */
	IRVZ_ERR_OPEN = 2,      /* a file couldn't be opened or created */
	IRVZ_ERR_READ = 3,      /* reading failed (or the data is corrupt) */
	IRVZ_ERR_WRITE = 4,     /* writing failed */
	IRVZ_ERR_FORMAT = 5,    /* not a valid WIA/RVZ file */
	IRVZ_ERR_ARGS = 6,      /* invalid arguments or options */
	IRVZ_ERR_MEMORY = 7,    /* out of memory */
	IRVZ_ERR_INTERNAL = 8
};

typedef enum irvz_format {
	IRVZ_FORMAT_UNKNOWN = 0, /* a plain image (ISO/GCM) or anything else */
	IRVZ_FORMAT_WIA = 1,
	IRVZ_FORMAT_RVZ = 2
} irvz_format;

typedef enum irvz_compression {
	IRVZ_COMPRESSION_NONE = 0,
	IRVZ_COMPRESSION_PURGE = 1, /* WIA only */
	IRVZ_COMPRESSION_BZIP2 = 2,
	IRVZ_COMPRESSION_LZMA = 3,
	IRVZ_COMPRESSION_LZMA2 = 4,
	IRVZ_COMPRESSION_ZSTD = 5 /* RVZ only */
} irvz_compression;

/* ---- I/O callbacks ------------------------------------------------------------------------- */

/* Random-access input. read() fills exactly len bytes at offset and returns 0, nonzero on error.
 * It may be called from several threads at once (extraction with threads > 1). */
typedef struct irvz_reader {
	void *user;
	uint64_t size;
	int (*read)(void *user, uint64_t offset, void *buf, size_t len);
} irvz_reader;

/* Random-access output. Every callback returns 0 on success.
 *   begin  (optional) called first. Extraction passes the image size, compression passes 0
 *          (the size isn't known in advance).
 *   write  stores len bytes at offset. Extraction writes from several threads at once, in any
 *          order; compression writes from one thread, mostly in order, and finally goes back to
 *          offset 0 for the headers.
 *   finish (optional) called last with the final size. */
typedef struct irvz_writer {
	void *user;
	int (*begin)(void *user, uint64_t size);
	int (*write)(void *user, uint64_t offset, const void *buf, size_t len);
	int (*finish)(void *user, uint64_t final_size);
} irvz_writer;

/* Progress in input bytes. Always called on the thread that called the irvz function. Return
 * nonzero to cancel. May be NULL. */
typedef int (*irvz_progress)(void *user, uint64_t done, uint64_t total);

/* ---- Memory helpers ------------------------------------------------------------------------ */

/* A growable buffer filled by irvz_writer_from_buffer() writers. Zero-initialise it; release
 * the data with irvz_buffer_free(). */
typedef struct irvz_buffer {
	uint8_t *data;
	uint64_t size;
	uint64_t capacity;
} irvz_buffer;

/* A reader over memory that stays valid (and unchanged) while it's used. */
IRVZ_API void irvz_reader_from_memory(irvz_reader *reader, const void *data, uint64_t size);
/* A writer that fills buffer (growing it as needed). */
IRVZ_API void irvz_writer_from_buffer(irvz_writer *writer, irvz_buffer *buffer);
IRVZ_API void irvz_buffer_free(irvz_buffer *buffer);

/* ---- Inspecting and random access ---------------------------------------------------------- */

typedef struct irvz_info {
	irvz_format format;
	irvz_compression compression;
	int compression_level;
	uint32_t chunk_size;
	uint64_t iso_size;  /* size of the decompressed image */
	uint64_t file_size; /* size of the WIA/RVZ file */
	uint32_t disc_type; /* 0 unknown, 1 GameCube, 2 Wii */
	uint32_t version;   /* format version, 0x01000000 = 1.0 */
	uint32_t version_compatible;
	char game_id[7];    /* "GALE01", NUL-terminated */
	char title[65];     /* internal name from the disc header, NUL-terminated */
} irvz_info;

/* Identifies a file from its first 4 bytes (or more). */
IRVZ_API irvz_format irvz_detect(const void *header, size_t len);

typedef struct irvz_file irvz_file;

IRVZ_API int irvz_open(const char *utf8_path, irvz_file **out);
/* The reader (and what it reads) must stay valid until irvz_close(). */
IRVZ_API int irvz_open_reader(const irvz_reader *reader, irvz_file **out);
/* data is not copied and must stay valid until irvz_close(). */
IRVZ_API int irvz_open_memory(const void *data, uint64_t size, irvz_file **out);
IRVZ_API void irvz_close(irvz_file *file);

IRVZ_API int irvz_get_info(const irvz_file *file, irvz_info *info);
/* Reads decompressed bytes of the disc image. */
IRVZ_API int irvz_read(irvz_file *file, uint64_t offset, void *buf, size_t len);

/* ---- Whole images -------------------------------------------------------------------------- */

typedef struct irvz_compress_options {
	irvz_format format;           /* IRVZ_FORMAT_RVZ (default) or IRVZ_FORMAT_WIA */
	irvz_compression compression; /* default: Zstandard for RVZ, LZMA2 for WIA */
	int level;                    /* bzip2/LZMA/LZMA2 1-9, zstd -22..22; default 5 */
	uint32_t chunk_size;          /* RVZ: power of two from 32 KiB, or a multiple of 2 MiB;
	                                 WIA: a multiple of 2 MiB. Default 128 KiB (RVZ), 2 MiB (WIA) */
	int threads;                  /* 0 = one per CPU core */
} irvz_compress_options;

/* Fills in the defaults (Dolphin's) for the format. */
IRVZ_API void irvz_compress_options_init(irvz_compress_options *options, irvz_format format);

/* Compresses a disc image (plain, or itself WIA/RVZ: converts between them) into WIA/RVZ. */
IRVZ_API int irvz_compress(const irvz_reader *src, const irvz_writer *dst, const irvz_compress_options *options,
                           irvz_progress progress, void *user);
IRVZ_API int irvz_compress_file(const char *src_path, const char *dst_path, const irvz_compress_options *options,
                                irvz_progress progress, void *user);
/* src: a plain or WIA/RVZ image in memory; the result is appended to an empty out buffer. */
IRVZ_API int irvz_compress_memory(const void *src, uint64_t size, irvz_buffer *out,
                                  const irvz_compress_options *options, irvz_progress progress, void *user);

/* Decompresses a whole WIA/RVZ image (threads: 0 = one per CPU core). */
IRVZ_API int irvz_extract(irvz_file *file, const irvz_writer *dst, int threads, irvz_progress progress, void *user);
IRVZ_API int irvz_extract_file(const char *src_path, const char *dst_path, int threads, irvz_progress progress,
                               void *user);
IRVZ_API int irvz_extract_memory(const void *src, uint64_t size, irvz_buffer *out, int threads,
                                 irvz_progress progress, void *user);

/* ---- Misc ---------------------------------------------------------------------------------- */

IRVZ_API const char *irvz_version(void);
IRVZ_API const char *irvz_error_string(int code);
/* More detail about the last error on this thread ("" if none). */
IRVZ_API const char *irvz_last_error(void);
IRVZ_API const char *irvz_compression_name(irvz_compression compression);
/* Allowed levels for a method; *min > *max when it has none. */
IRVZ_API void irvz_compression_levels(irvz_compression compression, int *min, int *max);

#ifdef __cplusplus
}
#endif

#endif /* IRVZ_H */
