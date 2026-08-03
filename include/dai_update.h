/* dai_update.h - keeping a shipped build current.
 *
 * The editor is handed to people who will not rebuild it. When something is
 * fixed they should get the fix without being told to go and fetch it, which
 * means the program checks a manifest at startup, downloads what changed,
 * verifies it and replaces itself.
 *
 * Two manifest shapes live here:
 *
 * The multi-file one, the same shape the JARVIS helper uses:
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
 * and the flat single-exe one the editor's own download page publishes:
 *
 *   {
 *     "version": "2026.08.03-1",
 *     "sha256":  "ab12...",
 *     "size":    6921434,
 *     "url":     "/download/DaidalosEditor.exe"
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

/* ---- the flat single-exe manifest (what the editor itself consumes) ------ */

/* The download page publishes one JSON object describing ONE executable:
 * {"version":"2026.08.03-1","sha256":"...","size":6921434,"url":"/download/..."}.
 * The installed build remembers what it is in a sidecar file next to the exe,
 * "<name>.version" holding the sha256 of the build in place. No version number
 * is baked into the binary, so a build never has to be told what it is - the
 * hash answers that. */
typedef struct dai_self_update {
    char     version[32];    /* the remote build's version string  */
    char     sha256[65];     /* sha256 the remote build must have  */
    char     url[512];       /* download URL, made absolute        */
    uint64_t size;           /* 0 when the manifest did not say    */
    int      needed;         /* 1 = the running install differs    */
    int      sidecar;        /* 1 = the sidecar existed and agreed */
} dai_self_update;

/* Fetches and parses the flat manifest, resolves a relative "url" against the
 * manifest's own address, and decides whether THIS exe is that build. The
 * sidecar says what was installed; when it is missing the exe itself is
 * hashed, so a fresh download of the current build is not downloaded again.
 *
 * Returns DAI_OK even when nothing needs doing - read out->needed. An
 * unreachable server is an error return, never a hang: the built in transport
 * gives the manifest a short timeout (see DAI_SELF_UPDATE_MANIFEST_TIMEOUT_MS). */
#define DAI_SELF_UPDATE_MANIFEST_TIMEOUT_MS 3000
#define DAI_SELF_UPDATE_DOWNLOAD_TIMEOUT_MS 60000
DAI_API dai_result dai_self_update_check(const char *manifest_url, const char *exe_path,
                                         dai_self_update *out, char *err, size_t err_len);

/* Downloads info->url, refuses anything whose sha256 or size disagrees with
 * the manifest, and stages the verified bytes as "<exe_path>.new". The live
 * exe is not touched - a running editor cannot be overwritten on Windows, and
 * should not be surprised either. */
DAI_API dai_result dai_self_update_stage(const dai_self_update *info, const char *exe_path,
                                         char *err, size_t err_len);

/* Hands the staged build to a tiny batch file that waits for this process to
 * exit, moves "<exe>.new" over the exe, writes the sidecar and starts the exe
 * again. The batch file deletes itself when done. Call this as the LAST thing
 * before the process exits - everything after the launch happens without us.
 *
 * Windows only: the shipped path is a Windows exe, and a POSIX build is a
 * developer build that updates with git, not a downloader. */
DAI_API dai_result dai_self_update_restart(const dai_self_update *info, const char *exe_path,
                                           char *err, size_t err_len);

/* Writes the sidecar ("<exe>.version" containing the sha256). Used when a
 * check found the running build already current but the sidecar was missing,
 * so the next check is cheap. */
DAI_API dai_result dai_self_update_mark_current(const char *exe_path, const char *sha256);

#ifdef __cplusplus
}
#endif
#endif /* DAI_UPDATE_H */
