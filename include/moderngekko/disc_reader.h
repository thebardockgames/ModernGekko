/* Read-only access to GameCube/Wii disc images (ISO, RVZ, WIA, WBFS, GCZ,
 * CISO) through Dolphin's DiscIO, as a plain C ABI so standalone tools such
 * as the BT3 viewer can load moderngekko_disc.dll at run time.
 *
 * Paths are those of the game partition's file system ("files/..." and
 * "sys/..." are accepted, as in an extracted folder): "files/wzs3us1.afs",
 * "sys/main.dol", "sys/fst.bin". Nothing is ever written to the image. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(MODERNGEKKO_DISC_BUILD)
#define MODERNGEKKO_DISC_API __declspec(dllexport)
#elif defined(_WIN32)
#define MODERNGEKKO_DISC_API __declspec(dllimport)
#else
#define MODERNGEKKO_DISC_API
#endif

#define MODERNGEKKO_DISC_ABI_VERSION 1

typedef struct MgDisc MgDisc;

MODERNGEKKO_DISC_API uint32_t mg_disc_abi_version(void);
/* UTF-8 path. Returns NULL and fills `error` (may be NULL) on failure. */
MODERNGEKKO_DISC_API MgDisc* mg_disc_open(const char* path, char* error, size_t error_size);
MODERNGEKKO_DISC_API void mg_disc_close(MgDisc* disc);
/* 0 = GameCube, 1 = Wii. */
MODERNGEKKO_DISC_API int mg_disc_platform(const MgDisc* disc);
/* Six-character game id (e.g. "RDSE70"), NUL-terminated into `out`. */
MODERNGEKKO_DISC_API void mg_disc_game_id(const MgDisc* disc, char* out, size_t out_size);
/* File size in bytes, or -1 if the path does not name a file. */
MODERNGEKKO_DISC_API int64_t mg_disc_file_size(const MgDisc* disc, const char* path);
/* Reads `size` bytes at `offset` of a file. Returns 1 only if all bytes were read. */
MODERNGEKKO_DISC_API int mg_disc_read_file(const MgDisc* disc, const char* path, uint64_t offset,
                                           void* buffer, uint64_t size);
/* Raw read of the game partition (decrypted), by partition offset. Returns 1 on success. */
MODERNGEKKO_DISC_API int mg_disc_read_partition(const MgDisc* disc, uint64_t offset, void* buffer,
                                                uint64_t size);

#ifdef __cplusplus
}
#endif
