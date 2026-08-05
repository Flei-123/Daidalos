/* dai_vfs.h - where bytes come from, in the editor and in a shipped game.
 *
 * The editor reads a project off the disk: scenes/main.daidalos is a file, a
 * script is a file, a model is a file. A shipped game has no project folder -
 * it is ONE executable someone was handed, and everything the game needs has
 * to be inside it. Those two situations must not produce two loaders, or the
 * export becomes the place where things silently stop working.
 *
 * So there is one read path with two backings:
 *
 *   dai_vfs_mount_dir("MyGame")            the editor: the project on disk
 *   dai_vfs_mount_archive(NULL, ...)       the export: the archive glued to
 *                                          the end of the running executable
 *
 * and after that everything asks the same question:
 *
 *   size_t n; void *b = dai_vfs_read("scenes/main.daidalos", &n);
 *
 * Paths are always '/' separated, relative, and never escape the mount:
 * ".." is rejected outright rather than normalised, because the only thing a
 * ".." in a scene file can be is someone else's idea.
 *
 * This is the Godot arrangement, not the Unity one. Nothing is compiled at
 * export time: a prebuilt runtime binary is copied, the project is packed into
 * one archive, and the archive is APPENDED to the copy. The loader finds it by
 * reading the last 16 bytes of its own file - a magic and the offset the
 * archive starts at - so the same bytes work as a standalone .dpk file and as
 * a tail glued onto an .exe.
 *
 * ---------------------------------------------------------------------------
 * THE ARCHIVE FORMAT (all integers little endian, which every target here is)
 * ---------------------------------------------------------------------------
 *
 * An archive begins at absolute file offset `base` (0 for a bare .dpk, the
 * padded size of the runtime for an .exe) and `base` is always a multiple of 4.
 *
 *   HEADER, 32 bytes, at base:
 *     +0   char[8]  "DAIPACK1"      magic
 *     +8   uint32   version         1
 *     +12  uint32   entry_count
 *     +16  uint32   dir_offset      directory, relative to base
 *     +20  uint32   dir_bytes       directory size in bytes
 *     +24  uint32   data_offset     first payload, relative to base
 *     +28  uint32   archive_bytes   header + payloads + directory + trailer
 *
 *   PAYLOADS: entry_count blobs. Each starts at a 4 byte aligned offset
 *   relative to base; the padding between them is zero. Stored raw - no
 *   compression. (A .glb is already compressed geometry and a scene file is a
 *   few kB; deflating either buys less than the decoder costs.)
 *
 *   DIRECTORY, at base + dir_offset, entry_count records, each 4 byte aligned:
 *     +0   uint32   path_len        bytes, WITHOUT a terminator
 *     +4   uint32   offset          payload, relative to base
 *     +8   uint32   length          payload size in bytes
 *     +12  uint32   hash            FNV-1a 32 of the payload (0 length -> 0)
 *     +16  char[path_len]           the path, then zero padding to a multiple
 *                                   of 4
 *   Sorted by path (byte order), so a lookup is a binary search.
 *
 *   TRAILER, the LAST 16 bytes of the file:
 *     +0   uint64   base            absolute offset of the header
 *     +8   char[8]  "DAIPACKE"      magic
 *
 * Reading is therefore: seek to end-16, check the magic, take `base`, seek
 * there, check "DAIPACK1", read the directory. Nothing has to know whether it
 * is looking at a .dpk or at an .exe with a tail.
 *
 * WHY the trailer AND the header: the header alone cannot be found from the
 * end of a file, and a trailer alone cannot be validated when the file is
 * handed over on its own. Both cost 48 bytes together and every corrupted
 * download fails loudly instead of loading half a scene.
 */
#ifndef DAI_VFS_H
#define DAI_VFS_H

#include "daidalos.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAI_PACK_MAGIC         "DAIPACK1"
#define DAI_PACK_TRAILER_MAGIC "DAIPACKE"
#define DAI_PACK_VERSION       1u
#define DAI_PACK_HEADER_BYTES  32u
#define DAI_PACK_TRAILER_BYTES 16u
#define DAI_PACK_ALIGN         4u
/* The startup description, packed as an ordinary entry so that `dai_pack -l`
 * shows it and a corrupt one fails like any other missing file. */
#define DAI_PACK_BOOT_PATH     "boot.cfg"
#define DAI_VFS_PATH_MAX       224

/* ---- mounting ----------------------------------------------------------
 *
 * Sources are searched by priority, HIGHEST first; among equal priorities the
 * one mounted LAST wins. That is the mod rule Mnemosyne already uses, and it
 * means a shipped game can mount its own archive at 0 and a patch folder at
 * 10 without anything else changing.
 *
 * Not thread safe against concurrent mounting. Reads are safe from several
 * threads once the mounts are set up: every read opens its own FILE.
 */

/* Mounts a real directory. Fails if it is not one. */
DAI_API dai_result dai_vfs_mount_dir(const char *dir, int priority);

/* Mounts an archive. `path` may be:
 *     NULL or "self"   the running executable (this is what a shipped game
 *                      does: its archive is its own tail)
 *     "game.dpk"       a standalone archive
 *     "MyGame.exe"     any file with an archive appended
 *
 * Returns DAI_ERR_FILE with a reason in `err` when the file has no archive -
 * which is the NORMAL answer for an unexported runtime binary, and the signal
 * to fall back to a directory mount. */
DAI_API dai_result dai_vfs_mount_archive(const char *path, int priority,
                                         char *err, size_t err_len);

DAI_API void     dai_vfs_unmount_all(void);
DAI_API uint32_t dai_vfs_mount_count(void);
/* "dir:<path>" or "pack:<path>", for a log line that says where a game is
 * actually reading from. Points at a static buffer, rebuilt per call. */
DAI_API const char *dai_vfs_mount_name(uint32_t index);

/* ---- reading ------------------------------------------------------------ */

/* Reads a whole file. Returns NULL when it is in no mount.
 *
 * The buffer is malloc'd with ONE extra zero byte past the end, so the result
 * can be handed straight to anything that wants a C string (a scene file, a
 * script) without a copy. *out_size does NOT count that byte. Free it with
 * dai_vfs_free. */
DAI_API void *dai_vfs_read(const char *path, size_t *out_size);
DAI_API void  dai_vfs_free(void *bytes);

DAI_API int dai_vfs_exists(const char *path);
/* Size in bytes, or -1 when it does not exist. */
DAI_API int64_t dai_vfs_size(const char *path);

/* Every path in every mount, sorted and de-duplicated. Fills up to `max`
 * entries of `stride` bytes and returns how many exist. */
DAI_API uint32_t dai_vfs_list(char *out, uint32_t max, uint32_t stride);

/* The real path on disk, when the file came from a directory mount. 0 for a
 * file that only exists inside an archive - which is the point: a caller that
 * needs a path (an old loader that only takes one) has to be told no rather
 * than handed something that does not exist. */
DAI_API int dai_vfs_real_path(const char *path, char *out, size_t out_len);

DAI_API const char *dai_vfs_last_error(void);

/* Absolute path of the running executable. Empty and 0 when the platform will
 * not say. */
DAI_API int dai_vfs_self_path(char *out, size_t out_len);

/* ---- the startup description -------------------------------------------
 *
 * What a double clicked .exe needs to know before it can do anything: which
 * scene, how big a window, what to call it. Packed as DAI_PACK_BOOT_PATH.
 * Text, one "key value" per line, because a binary struct in an archive is a
 * version number waiting to be forgotten. */
typedef struct dai_boot_config {
    char  scene[DAI_VFS_PATH_MAX];  /* archive path of the startup scene    */
    char  title[128];               /* window title                         */
    int   width, height;
    int   fullscreen;               /* 1 = borderless full screen by default */
    int   msaa;                     /* samples, 1 = off                     */
    int   tick_hz;
    int   max_bodies;
    int   physics_backend;          /* dai_physics_backend                  */
    float gravity[3];
} dai_boot_config;

DAI_API dai_boot_config dai_boot_config_default(void);
/* Writes the text form into `buf`; returns the number of bytes it needs
 * (excluding the terminator), so a small buffer is a size query. */
DAI_API size_t     dai_boot_config_write(const dai_boot_config *c, char *buf, size_t buf_size);
/* Unknown keys are skipped, like project settings and unlike scenes: a newer
 * exporter writing one more line must not stop an older runtime. */
DAI_API dai_result dai_boot_config_parse(const char *text, size_t len, dai_boot_config *out);

/* ---- writing archives ---------------------------------------------------
 *
 *   dai_pack *p = dai_pack_begin("MyGame.exe", "runtime_template.exe", e, sizeof e);
 *   dai_pack_add_file(p, "scenes/main.daidalos", "MyGame/scenes/main.daidalos");
 *   dai_pack_add_mem(p, DAI_PACK_BOOT_PATH, cfg_text, cfg_len);
 *   dai_pack_end(p, e, sizeof e);
 *
 * With `prefix_file` the output is that file plus the archive - the export.
 * Without it the output is a bare .dpk. Everything is written to a temporary
 * file and renamed into place, so an interrupted export cannot leave a half
 * written .exe that looks finished. */
typedef struct dai_pack dai_pack;

DAI_API dai_pack *dai_pack_begin(const char *out_path, const char *prefix_file,
                                 char *err, size_t err_len);
/* 1 on success. A duplicate virtual path, a path with "..", or an unreadable
 * source file is a failure and is remembered: dai_pack_end then refuses. */
DAI_API int      dai_pack_add_file(dai_pack *p, const char *vpath, const char *disk_path);
DAI_API int      dai_pack_add_mem(dai_pack *p, const char *vpath, const void *bytes, size_t len);
DAI_API uint32_t dai_pack_count(const dai_pack *p);
/* Finishes and closes. The handle is freed either way - on failure nothing is
 * left behind but the error. */
DAI_API dai_result dai_pack_end(dai_pack *p, char *err, size_t err_len);
DAI_API void       dai_pack_abort(dai_pack *p);

/* ---- inspecting an archive --------------------------------------------- */

typedef struct dai_pack_entry {
    char     path[DAI_VFS_PATH_MAX];
    uint64_t offset;      /* absolute, inside the file  */
    uint32_t length;
    uint32_t hash;
} dai_pack_entry;

typedef struct dai_pack_info {
    uint64_t base;          /* where the archive starts inside the file */
    uint64_t file_bytes;
    uint32_t version;
    uint32_t entry_count;
    uint32_t archive_bytes;
    uint32_t payload_bytes; /* the sum of every entry's length          */
} dai_pack_info;

/* Reads the header and directory of a .dpk or of an .exe with an archive
 * appended. Fills up to `max` entries and returns the number the archive HAS,
 * or 0 with a reason in `err`. Pass out = NULL, max = 0 to just get `info`. */
DAI_API uint32_t dai_pack_read(const char *file, dai_pack_info *info,
                               dai_pack_entry *out, uint32_t max,
                               char *err, size_t err_len);

/* Re-reads every payload and checks it against the stored hash. Returns the
 * number of BAD entries; 0 means the archive is intact. */
DAI_API uint32_t dai_pack_verify(const char *file, char *err, size_t err_len);

/* FNV-1a 32, the hash in the directory. Exposed so a test can compute the
 * expected value without linking the whole reader. */
DAI_API uint32_t dai_pack_hash(const void *bytes, size_t len);

/* ---- building an archive out of a project directory ---------------------
 *
 * (Implemented in src/dai_export.cpp - it walks directories, which the reader
 * has no business knowing how to do.)
 *
 * Packs, from <project_dir>:
 *     scenes/<every file>          the startup scene and the rest
 *     assets/<every file, recursive>
 *     settings/project.txt
 *     project.daidalos
 *     boot.cfg                     generated from `boot`, not copied
 *
 * cache/ is skipped on purpose: it is derived data by definition. Files
 * starting with '.' are skipped, which keeps .git and editor droppings out.
 *
 * `boot` may be NULL for the defaults. Returns the number of entries packed,
 * 0 with a reason in `err`. */
DAI_API uint32_t dai_pack_build_from_dir(const char *project_dir, const char *out_path,
                                         const char *prefix_file, const dai_boot_config *boot,
                                         char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* DAI_VFS_H */
