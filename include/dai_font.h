/*
 * TrueType font loading and glyph rasterisation - written from scratch, like
 * the PNG decoder and the glTF importer.
 *
 * Why not FreeType: it is 200k lines, it is a build dependency on every
 * platform you port to, and 95% of what a game needs from it is "give me the
 * outline of this glyph and its metrics". That part is a few hundred lines.
 *
 * What this does:
 *   parses cmap (formats 4 and 12), head, hhea, hmtx, maxp, loca, glyf
 *   simple and composite glyphs, quadratic beziers flattened adaptively
 *   scanline fill with 4x vertical supersampling and exact horizontal coverage
 *   packs the glyphs you ask for into one greyscale atlas, ready to upload
 *
 * What it does not do (yet): hinting, kerning tables (GPOS), colour glyphs,
 * font fallback chains. Advances and cmap lookups are exact.
 */
#ifndef DAI_FONT_H
#define DAI_FONT_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_font dai_font;

typedef struct dai_glyph {
    uint32_t codepoint;
    float    u0, v0, u1, v1;   /* atlas coordinates, 0..1                     */
    float    x0, y0, x1, y1;   /* offsets from the pen position, in pixels    */
    float    advance;          /* pen movement, in pixels                     */
} dai_glyph;

/* Loads a .ttf/.otf(glyf) and rasterises `pixel_height` sized glyphs for the
 * given codepoint ranges into one atlas. Pass ranges = NULL for ASCII 32..126
 * plus the Latin-1 supplement, which covers German, French and Spanish. */
DAI_API dai_font *dai_font_load(const char *path, float pixel_height,
                                const uint32_t *ranges, uint32_t range_pairs,
                                char *err, size_t err_len);
DAI_API void      dai_font_free(dai_font *f);

/* Loads whatever the system's UI font is, without the caller knowing where that
 * lives. A hard coded /usr/share/fonts path is a program that builds for
 * Windows, runs, and draws an interface with no text in it - which is exactly
 * how the editor first arrived there.
 *
 * Order: $DAI_FONT if set, then the usual Windows faces, then the usual Linux
 * and macOS ones. Returns NULL only if none of them exist, and writes the list
 * it tried into err so the failure is actionable. */
DAI_API dai_font *dai_font_load_ui(float pixel_height, char *err, size_t err_len);

/* The atlas: 8 bit coverage, tightly packed. Upload it as a texture. */
DAI_API const uint8_t *dai_font_atlas(const dai_font *f, uint32_t *width, uint32_t *height);
/* Same pixels expanded to RGBA (white, alpha = coverage) for renderers that
 * would rather not deal with a single channel format. Caller frees nothing;
 * the buffer belongs to the font. */
DAI_API const uint8_t *dai_font_atlas_rgba(dai_font *f, uint32_t *width, uint32_t *height);

DAI_API const dai_glyph *dai_font_glyph(const dai_font *f, uint32_t codepoint);
DAI_API float dai_font_line_height(const dai_font *f);
DAI_API float dai_font_ascent(const dai_font *f);
/* Width of a UTF-8 string in pixels, and the number of glyphs it needs. */
DAI_API float dai_font_measure(const dai_font *f, const char *utf8, uint32_t *glyph_count);
/* Decodes one UTF-8 code point, advancing *offset. Returns 0 at the end. */
DAI_API uint32_t dai_utf8_next(const char *s, uint32_t *offset);

#ifdef __cplusplus
}
#endif

#endif /* DAI_FONT_H */
