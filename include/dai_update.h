/* dai_update.h - keeping a shipped build current.
 *
 * The editor is handed to people who will not rebuild it. When something is
 * fixed they should get the fix without being told to go and fetch it, which
 * means the program checks a manifest at startup, downloads what changed,
 * verifies it and replaces itself.
 *
 * The manifest is the same shape the JARVIS helper already uses, so one
 * publishing habit covers both:
 *
 *   {
 *     "version": "0.2.0",
 *     "base_url": "https://jarvis.fleitec.com/dai/",
 *     "files": {
 *       "daidalos_editor.exe": { "sha256": "ab12...", "size": 4711 },
 *       "shaders.pack":        "cd34..."
 *     }
 *   }
 *
 * A file's value may be the hash on its own or an object with the size too.
 *
 * Every download is checked against its hash before it is allowed near the
 * install directory. An update that arrives corrupt is an update that does not
 * happen - the alternative is replacing a working editor with a broken one,
 * which is worse than being out of date.
 *
 * Nothing here talks to the network by itself in a test: dai_update_set_fetch
 * replaces the transport, so the logic can be exercised without a server.
 */
#ifndef DAI_UPDATE_H
#define DAI_UPDATE_H

#include "daidalos.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAI_UPDATE_MAX_FILES 16

typedef struct dai_update_file {
    char     name[128];
    char     sha256[65];     /* lower case hex, from the manifest */
    uint64_t size;           /* 0 when the manifest did not say   */
    int      needed;         /* 1 = local copy missing or differs  */
} dai_update_file;

typedef struct dai_update_info {
    char            version[32];
    char            base_url[256];
    dai_update_file files[DAI_UPDATE_MAX_FILES];
    uint32_t        file_count;
    uint32_t        needed_count;   /* how many files actually differ */
    int             available;      /* 1 = remote version is newer    */
} dai_update_info;

/* ---- hashing ------------------------------------------------------------ */

/* SHA-256 of a buffer, written as 64 lower case hex characters plus a NUL.
 * `out` must hold 65 bytes. */
DAI_API void dai_sha256_hex(const void *data, size_t size, char *out);

/* Same for a file. Returns DAI_ERR_NOT_FOUND if it cannot be read. */
DAI_API dai_result dai_sha256_file(const char *path, char *out);

/* ---- transport ---------------------------------------------------------- */

/* Fetches a URL. Must return 1 and malloc'd bytes into *out_bytes on success;
 * the caller frees with free(). Returning 0 means "could not fetch", which is
 * never fatal - an update that cannot be reached is simply skipped. */
typedef int (*dai_update_fetch_fn)(const char *url, void **out_bytes, size_t *out_size,
                                   void *user);

/* Replaces the built in transport. Pass NULL to restore it. Used by the tests
 * to run the whole flow against bytes held in memory. */
DAI_API void dai_update_set_fetch(dai_update_fetch_fn fn, void *user);

/* ---- the flow ----------------------------------------------------------- */

/* Downloads and parses the manifest, compares `current_version` against it and
 * hashes the local copy of each listed file to decide what is actually needed.
 *
 * `install_dir` is where the local files live - usually the directory the
 * executable is in. Returns DAI_OK even when nothing is available; check
 * out->available and out->needed_count. */
DAI_API dai_result dai_update_check(const char *manifest_url, const char *current_version,
                                    const char *install_dir, dai_update_info *out,
                                    char *err, size_t err_len);

/* Downloads every needed file into `install_dir`, verifying each hash before it
 * is written. Files land as "<name>.new"; nothing existing is touched, so an
 * interrupted download cannot leave a half written editor behind.
 *
 * Returns the number of files staged, 0 on failure. */
DAI_API uint32_t dai_update_download(const dai_update_info *info, const char *install_dir,
                                     char *err, size_t err_len);

/* Moves the staged files into place. `running_exe` may name the executable that
 * is currently running: Windows refuses to overwrite it but allows renaming it
 * out of the way, so it is moved to "<name>.old" first and deleted on the next
 * start by dai_update_sweep.
 *
 * Call this at exit, or immediately before restarting. */
DAI_API dai_result dai_update_commit(const dai_update_info *info, const char *install_dir,
                                     const char *running_exe, char *err, size_t err_len);

/* Deletes leftover "*.old" files. Call once at startup, before anything else -
 * it is the other half of the rename trick and costs nothing when there is
 * nothing to sweep. */
DAI_API void dai_update_sweep(const char *install_dir);

/* Compares two dotted version strings numerically: <0, 0 or >0.
 * "0.10.0" is newer than "0.9.9", which strcmp gets wrong. */
DAI_API int dai_version_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
#endif /* DAI_UPDATE_H */
