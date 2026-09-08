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
 * Deliberately small. Blend modes and shader graphs are not here because the
 * renderer does not have them, and a file format that promises fields nothing
 * reads is a file format people stop trusting.
 *
 * What IS here, because the renderer does have it: the three maps of the
 * glTF metallic-roughness model (docs/MATERIALS.md), and the WORLD PROJECTION
 * that lets a wall built out of boxes and CSG wear one of them without a UV
 * unwrap:
 *
 *     daidalos-material 1
 *     color 0.82 0.8 0.76
 *     base_color_map Textures/raufaser_basecolor.png
 *     orm_map Textures/raufaser_orm.png
 *     normal_map Textures/raufaser_normal.png
 *     triplanar 1
 *     triplanar_scale 2
 *     triplanar_blend 4
 *
 * The maps are paths relative to the project's Assets folder, exactly like
 * every other asset reference in the document - never a handle, so the file
 * survives a reload, a copy to another machine and a rebuild of the renderer.
 */
#ifndef DAI_MATFILE_H
#define DAI_MATFILE_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough for "Textures/generated/raufaser_wand_basecolor.png" and then
 * some. A path that does not fit is a path that is truncated on the way in,
 * not one that silently loads the wrong file. */
#define DAI_MATFILE_PATH 160

typedef struct dai_matfile {
    dai_vec3 color;        /* linear 0..1, the base colour                   */
    float    roughness;    /* 0 mirror, 1 matte                              */
    float    metallic;     /* 0 dielectric, 1 metal - read by the renderer
                              when it grows one; stored so a file written
                              today does not need rewriting then            */
    float    emissive;     /* 0 = not a light source                         */

    /* The maps, relative to the project's Assets folder. Empty = no map in
     * that slot, which is a complete material, not a broken one. */
    char     base_color_map[DAI_MATFILE_PATH];   /* sRGB   albedo            */
    char     orm_map[DAI_MATFILE_PATH];          /* linear AO/rough/metal    */
    char     normal_map[DAI_MATFILE_PATH];       /* linear tangent space     */
    float    normal_strength;                    /* multiplies the map, 1    */

    /* World projection. Off by default: it costs three samples per map, and a
     * material that came out of Blender has a UV set already. */
    int      triplanar;        /* 0 = sample the UV set, 1 = project in world */
    float    triplanar_scale;  /* METRES per repeat, not a repeat count       */
    float    triplanar_blend;  /* 1 wide wash .. 16 hard edge, default 4      */
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
/* The same, with every field written out even when it matches the default.
 * What a NEW material file should contain: something to edit. */
DAI_API size_t     dai_matfile_to_text_full(const dai_matfile *m, char *buf, size_t buf_size);

/* Is this path a material file? One place to ask, so the answer cannot drift
 * between the browser, the inspector and the loader. */
DAI_API int dai_matfile_is_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DAI_MATFILE_H */
