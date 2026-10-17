cmake_minimum_required(VERSION 3.20)

# The version lives in the public header.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/irvz/irvz.h IRVZ_VERSION_DEFINES
	REGEX "^#define IRVZ_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(part MAJOR MINOR PATCH)
	string(REGEX REPLACE ".*#define IRVZ_VERSION_${part} ([0-9]+).*" "\\1" IRVZ_VERSION_${part} "${IRVZ_VERSION_DEFINES}")
endforeach()

project(iRVZ
	VERSION ${IRVZ_VERSION_MAJOR}.${IRVZ_VERSION_MINOR}.${IRVZ_VERSION_PATCH}
	DESCRIPTION "WIA/RVZ disc image library and tool (Dolphin's WIABlob, standalone)"
	LANGUAGES C CXX)

get_directory_property(IRVZ_HAS_PARENT PARENT_DIRECTORY)
if(IRVZ_HAS_PARENT)
	set(IRVZ_TOPLEVEL OFF)
else()
	set(IRVZ_TOPLEVEL ON)
endif()

option(IRVZ_SHARED      "Build a shared library instead of a static one" OFF)
option(IRVZ_BUILD_CLI   "Build the irvz command line tool" ${IRVZ_TOPLEVEL})
option(IRVZ_BUILD_TESTS "Build the self-test" ${IRVZ_TOPLEVEL})

# Third-party libraries. The copies in third_party/ are built by default. A host project that
# already links its own can pass existing CMake targets instead, to avoid duplicate symbols:
#   IRVZ_ZSTD_TARGET  zstd (zstd.h)
#   IRVZ_LZMA_TARGET  LZMA SDK with LzmaDec/LzmaEnc/Lzma2Dec/Lzma2Enc, included as "lzma/C/..."
#   IRVZ_BZIP2_TARGET libbzip2 (bzlib.h); define bz_internal_error yourself if built BZ_NO_STDIO
set(IRVZ_ZSTD_TARGET "" CACHE STRING "Existing CMake target providing zstd")
set(IRVZ_LZMA_TARGET "" CACHE STRING "Existing CMake target providing the LZMA SDK (with LZMA2)")
set(IRVZ_BZIP2_TARGET "" CACHE STRING "Existing CMake target providing bzip2")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(MSVC)
	set(IRVZ_WARN_OFF /w)
else()
	set(IRVZ_WARN_OFF -w)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# zstd ---------------------------------------------------------------------------------------
if(IRVZ_ZSTD_TARGET)
	set(IRVZ_ZSTD ${IRVZ_ZSTD_TARGET})
else()
	file(GLOB IRVZ_ZSTD_SOURCES ${TP}/zstd/lib/common/*.c ${TP}/zstd/lib/compress/*.c ${TP}/zstd/lib/decompress/*.c)
	add_library(irvz_zstd STATIC ${IRVZ_ZSTD_SOURCES})
	target_include_directories(irvz_zstd PUBLIC $<BUILD_INTERFACE:${TP}/zstd/lib>)
	target_compile_definitions(irvz_zstd PRIVATE XXH_NAMESPACE=ZSTD_ DEBUGLEVEL=0 ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0
		ZSTDLIB_VISIBLE= ZSTDERRORLIB_VISIBLE= ZDICTLIB_VISIBLE=)
	target_compile_options(irvz_zstd PRIVATE ${IRVZ_WARN_OFF})
	set(IRVZ_ZSTD irvz_zstd)
endif()

# LZMA SDK (single-threaded LZMA + LZMA2) ----------------------------------------------------
if(IRVZ_LZMA_TARGET)
	set(IRVZ_LZMA ${IRVZ_LZMA_TARGET})
else()
	add_library(irvz_lzma STATIC
		${TP}/lzma/C/CpuArch.c ${TP}/lzma/C/LzFind.c ${TP}/lzma/C/LzFindOpt.c
		${TP}/lzma/C/LzmaDec.c ${TP}/lzma/C/LzmaEnc.c ${TP}/lzma/C/Lzma2Dec.c ${TP}/lzma/C/Lzma2Enc.c)
	target_include_directories(irvz_lzma PUBLIC $<BUILD_INTERFACE:${TP}>)
	target_compile_definitions(irvz_lzma PUBLIC Z7_ST)
	target_compile_options(irvz_lzma PRIVATE ${IRVZ_WARN_OFF})
	set(IRVZ_LZMA irvz_lzma)
endif()

# bzip2 (stream API only) --------------------------------------------------------------------
if(IRVZ_BZIP2_TARGET)
	set(IRVZ_BZIP2 ${IRVZ_BZIP2_TARGET})
else()
	add_library(irvz_bzip2 STATIC
		${TP}/bzip2/blocksort.c ${TP}/bzip2/bzlib.c ${TP}/bzip2/compress.c ${TP}/bzip2/crctable.c
		${TP}/bzip2/decompress.c ${TP}/bzip2/huffman.c ${TP}/bzip2/randtable.c)
	target_include_directories(irvz_bzip2 PUBLIC $<BUILD_INTERFACE:${TP}/bzip2>)
	target_compile_definitions(irvz_bzip2 PUBLIC BZ_NO_STDIO)
	target_compile_options(irvz_bzip2 PRIVATE ${IRVZ_WARN_OFF})
	set(IRVZ_BZIP2 irvz_bzip2)
endif()

# iRVZ ---------------------------------------------------------------------------------------
find_package(Threads REQUIRED)

set(IRVZ_SOURCES
	src/compression.cpp
	src/crypto.cpp
	src/disc.cpp
	src/io.cpp
	src/irvz.cpp
	src/lfg.cpp
	src/wia.cpp)

if(IRVZ_SHARED)
	add_library(irvz SHARED ${IRVZ_SOURCES})
	target_compile_definitions(irvz PUBLIC IRVZ_SHARED PRIVATE IRVZ_BUILDING)
else()
	add_library(irvz STATIC ${IRVZ_SOURCES})
endif()
add_library(iRVZ::iRVZ ALIAS irvz)
set_target_properties(irvz PROPERTIES OUTPUT_NAME iRVZ CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(irvz PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(irvz PRIVATE ${IRVZ_ZSTD} ${IRVZ_LZMA} ${IRVZ_BZIP2} PUBLIC Threads::Threads)
if(IRVZ_BZIP2_TARGET)
	target_compile_definitions(irvz PRIVATE IRVZ_EXTERNAL_BZIP2)
endif()
if(MSVC)
	target_compile_options(irvz PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus)
	target_compile_definitions(irvz PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
	target_compile_options(irvz PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(IRVZ_BUILD_CLI)
	add_executable(irvz_cli tools/irvz_cli.cpp)
	set_target_properties(irvz_cli PROPERTIES OUTPUT_NAME irvz CXX_STANDARD 20)
	target_link_libraries(irvz_cli PRIVATE irvz)
	if(MSVC)
		target_compile_definitions(irvz_cli PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	endif()
	install(TARGETS irvz_cli RUNTIME DESTINATION bin)
endif()

if(IRVZ_BUILD_TESTS)
	enable_testing()
	add_executable(irvz_selftest tests/selftest.cpp)
	set_target_properties(irvz_selftest PROPERTIES CXX_STANDARD 20)
	target_link_libraries(irvz_selftest PRIVATE irvz)
	if(NOT IRVZ_SHARED)
		# The test builds synthetic Wii discs with the library's own crypto
		target_include_directories(irvz_selftest PRIVATE src)
	endif()
	add_test(NAME selftest COMMAND irvz_selftest)
endif()

if(IRVZ_TOPLEVEL)
	install(TARGETS irvz ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
	install(FILES include/irvz/irvz.h DESTINATION include/irvz)
endif()
