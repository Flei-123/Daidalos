/*
 * SVG parsing and rasterisation - written from scratch, like the TrueType
 * loader next to it.
 *
 * Why: an editor needs icons, and the two usual answers are both bad. Baking
 * PNGs means every icon exists at exactly one size and looks like porridge on
 * a 150% display; linking a full SVG library (librsvg pulls in cairo, pango,
 * glib) means shipping a desktop stack to draw a play triangle. What an icon
 * actually needs from SVG is a path, a stroke width and a viewBox, and that
 * is a few hundred lines.
 *
 * What this does:
 *   an XML scanner good enough for icon files, not for documents
 *   <path> with M m L l H h V v C c S s Q q T t A a Z z, arcs included
 *   <rect> (rx/ry), <circle>, <ellipse>, <line>, <polyline>, <polygon>
 *   <g> with inherited presentation attributes
 *   transform= translate/scale/rotate/matrix/skewX/skewY, nested
 *   fill and fill-rule (nonzero, evenodd), stroke with width, caps and joins
 *   scanline fill, 4x vertical supersampling, exact horizontal coverage
 *
 * What it deliberately does not do: colour, gradients, patterns, text, CSS,
 * clip paths, opacity. These are UI icons - they are rasterised to COVERAGE
 * and tinted at draw time, so an icon's own colours would be thrown away
 * anyway. `fill="none"` and `stroke="none"` are honoured because those two
 * decide whether geometry exists at all.
 */
#ifndef DAI_SVG_H
#define DAI_SVG_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_svg dai_svg;

/* Parses an SVG document from memory. `text` need not be NUL terminated if
 * len is given; pass len = 0 to use strlen. Returns NULL and fills err on a
 * document with no drawable geometry. */
DAI_API dai_svg *dai_svg_parse(const char *text, size_t len, char *err, size_t err_len);
DAI_API dai_svg *dai_svg_load(const char *path, char *err, size_t err_len);
DAI_API void     dai_svg_free(dai_svg *s);

/* The viewBox, or the width/height attributes when there is none. */
DAI_API void dai_svg_viewbox(const dai_svg *s, float *x, float *y, float *w, float *h);
/* How many shapes survived parsing - a document that drew nothing is the
 * failure mode worth catching in a test. */
DAI_API uint32_t dai_svg_shape_count(const dai_svg *s);

/* Rasterises into an 8 bit coverage bitmap, w*h, origin top left, row major.
 * The viewBox is scaled to fit inside w-2*pad by h-2*pad and centred, so a
 * square icon in a square bitmap keeps its proportions. Returns 1 on success.
 */
DAI_API int dai_svg_rasterize(const dai_svg *s, uint8_t *out, int w, int h, float pad);

#ifdef __cplusplus
}
#endif

#endif /* DAI_SVG_H */
