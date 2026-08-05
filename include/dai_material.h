/*
 * Materials, as files.
 *
 * A material used to be a NAME from a fixed list - "Metal", "Glass" - and the
 * numbers that actually reached the renderer lived on the object. Two crates
 * that were meant to look the same were two independent sets of numbers, and
 * making them match again meant retyping them. That is not a material, it is a
 * label next to some values.
 *
 * So a material is a file, like a scene and like a prefab: one place the
 * numbers live, any number of objects pointing at it, and changing the file
 * changes all of them. It is text, in the same shape as every other format in
 * this engine - a header line, then only what differs from the default - which
 * means it diffs, it merges, and it can be edited in the editor's own script
 * tab without a special inspector for it.
 *
 *     daidalos-material 1
 *     color 0.82 0.24 0.20
 *     roughness 0.35
 *
 * Deliberately small. Textures, shader graphs and blend modes are not here
 * because the renderer does not have them yet, and a file format that promises
 * fields nothing reads is a file format people stop trusting.
 */
#ifndef DAI_MATFILE_H
#define DAI_MATFILE_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_matfile {
    dai_vec3 color;        /* linear 0..1, the base colour                   */
    float    roughness;    /* 0 mirror, 1 matte                              */
    float    metallic;     /* 0 dielectric, 1 metal - read by the renderer
                              when it grows one; stored so a file written
                              today does not need rewriting then            */
    float    emissive;     /* 0 = not a light source                         */
} dai_matfile;

DAI_API dai_matfile dai_matfile_default(void);

/* Reads a .daimat. A missing key keeps the default; an unknown key is skipped
 * rather than rejected, so a file written by a newer editor still loads. */
DAI_API dai_result dai_matfile_load(dai_matfile *out, const char *path,
                                     char *err, size_t err_size);
/* Writes one. Only fields that differ from the default are written, which is
 * what keeps the files short enough to read at a glance. */
DAI_API dai_result dai_matfile_save(const dai_matfile *m, const char *path);

/* The same two, without touching the disk - for hosts that already have the
 * bytes (a pack file, a network fetch) and for the tests. `to_text` returns
 * the number of bytes the text needs, excluding the terminator. */
DAI_API dai_result dai_matfile_from_text(dai_matfile *out, const char *text, size_t len);
DAI_API size_t     dai_matfile_to_text(const dai_matfile *m, char *buf, size_t buf_size);

/* Is this path a material file? One place to ask, so the answer cannot drift
 * between the browser, the inspector and the loader. */
DAI_API int dai_matfile_is_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DAI_MATFILE_H */
