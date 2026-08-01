// TrueType parsing and rasterisation. See include/dai_font.h for the why.

#include "dai_font.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- byte reader

struct Reader {
    const uint8_t *p = nullptr;
    size_t size = 0;
    bool ok = true;

    uint8_t  u8(size_t off) const { return off < size ? p[off] : 0; }
    uint16_t u16(size_t off) const { return off + 1 < size ? (uint16_t)((p[off] << 8) | p[off + 1]) : 0; }
    int16_t  s16(size_t off) const { return (int16_t)u16(off); }
    uint32_t u32(size_t off) const {
        return off + 3 < size ? ((uint32_t)p[off] << 24) | ((uint32_t)p[off+1] << 16) |
                                ((uint32_t)p[off+2] << 8) | p[off+3] : 0;
    }
};

struct Point { float x, y; bool on_curve; };

struct Contour { std::vector<Point> pts; };

// ---------------------------------------------------------------- rasteriser

// Scanline fill with 4x vertical supersampling. Horizontal coverage is exact
// (analytic span ends), vertical is sampled - which is the cheap half of what
// a real rasteriser does and is visually indistinguishable at UI sizes.
struct Edge { float x0, y0, x1, y1; int dir; };

void flatten_quad(std::vector<Point> &out, Point a, Point c, Point b) {
    // adaptive: more segments when the control point is far from the chord
    float dx = a.x - 2 * c.x + b.x, dy = a.y - 2 * c.y + b.y;
    float dev = std::sqrt(dx * dx + dy * dy);
    int n = (int)std::ceil(std::sqrt(dev * 2.0f));
    if (n < 2) n = 2;
    if (n > 24) n = 24;
    for (int i = 1; i <= n; ++i) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        out.push_back({ mt*mt*a.x + 2*mt*t*c.x + t*t*b.x,
                        mt*mt*a.y + 2*mt*t*c.y + t*t*b.y, true });
    }
}

void rasterise(const std::vector<Contour> &contours, int w, int h, float ox, float oy,
               std::vector<uint8_t> &out) {
    out.assign((size_t)w * h, 0);
    if (!w || !h) return;

    std::vector<Edge> edges;
    for (const Contour &c : contours) {
        for (size_t i = 0; i < c.pts.size(); ++i) {
            Point a = c.pts[i], b = c.pts[(i + 1) % c.pts.size()];
            float ax = a.x - ox, ay = a.y - oy, bx = b.x - ox, by = b.y - oy;
            if (ay == by) continue;
            edges.push_back({ ax, ay, bx, by, ay < by ? 1 : -1 });
        }
    }
    if (edges.empty()) return;

    const int SS = 4;
    std::vector<float> xs;
    std::vector<int> dirs;
    std::vector<uint16_t> acc((size_t)w, 0);

    for (int y = 0; y < h; ++y) {
        std::fill(acc.begin(), acc.end(), 0);
        for (int s = 0; s < SS; ++s) {
            float sy = (float)y + ((float)s + 0.5f) / SS;
            xs.clear(); dirs.clear();
            for (const Edge &e : edges) {
                float y0 = std::min(e.y0, e.y1), y1 = std::max(e.y0, e.y1);
                if (sy < y0 || sy >= y1) continue;
                float t = (sy - e.y0) / (e.y1 - e.y0);
                xs.push_back(e.x0 + (e.x1 - e.x0) * t);
                dirs.push_back(e.dir);
            }
            if (xs.size() < 2) continue;
            // sort crossings together with their winding direction
            std::vector<int> idx(xs.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
            std::sort(idx.begin(), idx.end(), [&](int a, int b) { return xs[a] < xs[b]; });

            int winding = 0;
            for (size_t i = 0; i + 1 < idx.size(); ++i) {
                winding += dirs[idx[i]];
                if (winding == 0) continue;            // non zero winding rule
                float xa = xs[idx[i]], xb = xs[idx[i + 1]];
                if (xb <= 0.0f || xa >= (float)w) continue;
                xa = std::max(xa, 0.0f); xb = std::min(xb, (float)w);
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) {
                    acc[(size_t)ia] += (uint16_t)((xb - xa) * (255.0f / SS));
                } else {
                    acc[(size_t)ia] += (uint16_t)(((float)(ia + 1) - xa) * (255.0f / SS));
                    for (int x = ia + 1; x < ib; ++x) acc[(size_t)x] += (uint16_t)(255.0f / SS);
                    if (ib < w) acc[(size_t)ib] += (uint16_t)((xb - (float)ib) * (255.0f / SS));
                }
            }
        }
        uint8_t *row = &out[(size_t)y * w];
        for (int x = 0; x < w; ++x) row[x] = (uint8_t)std::min<uint16_t>(acc[(size_t)x], 255);
    }
}

} // namespace

// ---------------------------------------------------------------- font

struct dai_font {
    std::vector<uint8_t> data;
    Reader r;
    size_t glyf = 0, loca = 0, cmap = 0, hmtx = 0;
    uint32_t glyf_len = 0;
    int loca_long = 0;
    uint16_t num_glyphs = 0, num_hmetrics = 0;
    float units_per_em = 1000.0f;
    float scale = 1.0f;
    float ascent = 0, descent = 0, line_gap = 0;

    std::vector<uint8_t> atlas;
    std::vector<uint8_t> atlas_rgba;
    uint32_t aw = 0, ah = 0;
    std::unordered_map<uint32_t, dai_glyph> glyphs;
};

namespace {

size_t find_table(const Reader &r, const char *tag) {
    uint16_t n = r.u16(4);
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = 12 + (size_t)i * 16;
        if (!std::memcmp(r.p + rec, tag, 4)) return r.u32(rec + 8);
    }
    return 0;
}

uint32_t glyph_index(const dai_font *f, uint32_t cp) {
    if (!f->cmap) return 0;
    const Reader &r = f->r;
    uint16_t n = r.u16(f->cmap + 2);
    size_t best = 0;
    int best_score = -1;
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = f->cmap + 4 + (size_t)i * 8;
        uint16_t plat = r.u16(rec), enc = r.u16(rec + 2);
        size_t off = f->cmap + r.u32(rec + 4);
        int score = (plat == 3 && enc == 10) ? 4 : (plat == 3 && enc == 1) ? 3 :
                    (plat == 0) ? 2 : 1;
        if (score > best_score) { best_score = score; best = off; }
    }
    if (!best) return 0;
    uint16_t format = r.u16(best);
    if (format == 4) {
        uint16_t segx2 = r.u16(best + 6);
        size_t ends = best + 14, starts = ends + segx2 + 2;
        size_t deltas = starts + segx2, ranges = deltas + segx2;
        for (uint16_t s = 0; s < segx2 / 2; ++s) {
            uint16_t end = r.u16(ends + (size_t)s * 2);
            if (cp > end) continue;
            uint16_t start = r.u16(starts + (size_t)s * 2);
            if (cp < start) return 0;
            uint16_t delta = r.u16(deltas + (size_t)s * 2);
            uint16_t range = r.u16(ranges + (size_t)s * 2);
            if (!range) return (uint16_t)(cp + delta);
            size_t addr = ranges + (size_t)s * 2 + range + (cp - start) * 2;
            uint16_t g = r.u16(addr);
            return g ? (uint16_t)(g + delta) : 0;
        }
        return 0;
    }
    if (format == 12) {
        uint32_t groups = r.u32(best + 12);
        for (uint32_t g = 0; g < groups; ++g) {
            size_t rec = best + 16 + (size_t)g * 12;
            uint32_t s = r.u32(rec), e = r.u32(rec + 4), gi = r.u32(rec + 8);
            if (cp >= s && cp <= e) return gi + (cp - s);
        }
    }
    return 0;
}

size_t glyph_offset(const dai_font *f, uint32_t gid, uint32_t *len) {
    if (gid >= f->num_glyphs) return 0;
    size_t a, b;
    if (f->loca_long) { a = f->r.u32(f->loca + (size_t)gid * 4); b = f->r.u32(f->loca + (size_t)gid * 4 + 4); }
    else { a = (size_t)f->r.u16(f->loca + (size_t)gid * 2) * 2; b = (size_t)f->r.u16(f->loca + (size_t)gid * 2 + 2) * 2; }
    if (b <= a) { *len = 0; return 0; }
    *len = (uint32_t)(b - a);
    return f->glyf + a;
}

// Reads one glyph's contours in font units. Composite glyphs recurse.
bool load_contours(const dai_font *f, uint32_t gid, std::vector<Contour> &out, int depth = 0) {
    if (depth > 4) return false;
    uint32_t len = 0;
    size_t off = glyph_offset(f, gid, &len);
    if (!off || !len) return true;                    // empty glyph, e.g. space
    const Reader &r = f->r;
    int16_t ncont = r.s16(off);

    if (ncont < 0) {                                   // composite
        size_t p = off + 10;
        for (;;) {
            uint16_t flags = r.u16(p), idx = r.u16(p + 2);
            p += 4;
            float dx, dy;
            if (flags & 1) { dx = (float)r.s16(p); dy = (float)r.s16(p + 2); p += 4; }
            else { dx = (float)(int8_t)r.u8(p); dy = (float)(int8_t)r.u8(p + 1); p += 2; }
            float a = 1, b = 0, c = 0, d = 1;
            if (flags & 8) { a = d = r.s16(p) / 16384.0f; p += 2; }
            else if (flags & 0x40) { a = r.s16(p) / 16384.0f; d = r.s16(p + 2) / 16384.0f; p += 4; }
            else if (flags & 0x80) {
                a = r.s16(p) / 16384.0f; b = r.s16(p+2) / 16384.0f;
                c = r.s16(p+4) / 16384.0f; d = r.s16(p+6) / 16384.0f; p += 8;
            }
            std::vector<Contour> sub;
            load_contours(f, idx, sub, depth + 1);
            for (Contour &ct : sub) {
                for (Point &pt : ct.pts) {
                    float x = pt.x, y = pt.y;
                    pt.x = a * x + c * y + dx;
                    pt.y = b * x + d * y + dy;
                }
                out.push_back(std::move(ct));
            }
            if (!(flags & 0x20)) break;
        }
        return true;
    }

    size_t p = off + 10;
    std::vector<uint16_t> ends((size_t)ncont);
    for (int i = 0; i < ncont; ++i) { ends[(size_t)i] = r.u16(p); p += 2; }
    uint16_t ins = r.u16(p); p += 2 + ins;
    uint32_t npts = (uint32_t)ends.back() + 1;

    std::vector<uint8_t> flags;
    flags.reserve(npts);
    while (flags.size() < npts) {
        uint8_t fl = r.u8(p++);
        flags.push_back(fl);
        if (fl & 8) { uint8_t rep = r.u8(p++); for (uint8_t i = 0; i < rep && flags.size() < npts; ++i) flags.push_back(fl); }
    }
    std::vector<float> xs(npts), ys(npts);
    int16_t v = 0;
    for (uint32_t i = 0; i < npts; ++i) {
        uint8_t fl = flags[i];
        if (fl & 2) { uint8_t d = r.u8(p++); v = (int16_t)((fl & 16) ? v + d : v - d); }
        else if (!(fl & 16)) { v = (int16_t)(v + r.s16(p)); p += 2; }
        xs[i] = (float)v;
    }
    v = 0;
    for (uint32_t i = 0; i < npts; ++i) {
        uint8_t fl = flags[i];
        if (fl & 4) { uint8_t d = r.u8(p++); v = (int16_t)((fl & 32) ? v + d : v - d); }
        else if (!(fl & 32)) { v = (int16_t)(v + r.s16(p)); p += 2; }
        ys[i] = (float)v;
    }

    uint32_t start = 0;
    for (int ci = 0; ci < ncont; ++ci) {
        uint32_t end = ends[(size_t)ci];
        Contour c;
        std::vector<Point> raw;
        for (uint32_t i = start; i <= end; ++i) raw.push_back({ xs[i], ys[i], (flags[i] & 1) != 0 });
        if (!raw.empty()) {
            // TrueType allows a contour to start off curve; synthesise a point
            if (!raw[0].on_curve) {
                Point mid = raw.back().on_curve ? raw.back()
                          : Point{ (raw[0].x + raw.back().x) * 0.5f, (raw[0].y + raw.back().y) * 0.5f, true };
                raw.insert(raw.begin(), mid);
            }
            c.pts.push_back(raw[0]);
            for (size_t i = 1; i <= raw.size(); ++i) {
                Point cur = raw[i % raw.size()];
                if (cur.on_curve) { c.pts.push_back(cur); continue; }
                Point next = raw[(i + 1) % raw.size()];
                Point endp = next.on_curve ? next
                           : Point{ (cur.x + next.x) * 0.5f, (cur.y + next.y) * 0.5f, true };
                flatten_quad(c.pts, c.pts.back(), cur, endp);
                if (next.on_curve) ++i;
            }
            out.push_back(std::move(c));
        }
        start = end + 1;
    }
    return true;
}

} // namespace

extern "C" {

uint32_t dai_utf8_next(const char *s, uint32_t *offset) {
    if (!s || !offset) return 0;
    const unsigned char *p = (const unsigned char *)s + *offset;
    if (!*p) return 0;
    uint32_t cp = 0;
    int extra = 0;
    if (*p < 0x80) { cp = *p; extra = 0; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
    else { ++*offset; return 0xFFFD; }
    for (int i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *offset += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *offset += (uint32_t)extra + 1;
    return cp;
}

dai_font *dai_font_load(const char *path, float pixel_height,
                        const uint32_t *ranges, uint32_t range_pairs,
                        char *err, size_t err_len) {
    auto bail = [&](const char *m) -> dai_font * {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return nullptr;
    };
    if (!path || pixel_height < 2.0f) return bail("bad arguments");

    FILE *fp = std::fopen(path, "rb");
    if (!fp) return bail("cannot open font file");
    dai_font *f = new dai_font();
    std::fseek(fp, 0, SEEK_END); long n = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
    f->data.resize((size_t)n);
    bool read_ok = std::fread(f->data.data(), 1, (size_t)n, fp) == (size_t)n;
    std::fclose(fp);
    if (!read_ok) { delete f; return bail("short read"); }

    f->r.p = f->data.data();
    f->r.size = f->data.size();
    uint32_t tag = f->r.u32(0);
    if (tag != 0x00010000 && tag != 0x74727565) { delete f; return bail("not a TrueType outline font"); }

    size_t head = find_table(f->r, "head"), hhea = find_table(f->r, "hhea");
    size_t maxp = find_table(f->r, "maxp");
    f->glyf = find_table(f->r, "glyf");
    f->loca = find_table(f->r, "loca");
    f->cmap = find_table(f->r, "cmap");
    f->hmtx = find_table(f->r, "hmtx");
    if (!head || !hhea || !maxp || !f->glyf || !f->loca || !f->cmap) { delete f; return bail("missing a required table"); }

    f->units_per_em = (float)f->r.u16(head + 18);
    if (f->units_per_em <= 0) f->units_per_em = 1000.0f;
    f->loca_long = f->r.s16(head + 50);
    f->num_glyphs = f->r.u16(maxp + 4);
    f->num_hmetrics = f->r.u16(hhea + 34);
    f->ascent = (float)f->r.s16(hhea + 4);
    f->descent = (float)f->r.s16(hhea + 6);
    f->line_gap = (float)f->r.s16(hhea + 8);
    f->scale = pixel_height / f->units_per_em;

    static const uint32_t kDefault[4] = { 32, 126, 160, 255 };
    if (!ranges || !range_pairs) { ranges = kDefault; range_pairs = 2; }

    // rasterise everything first, then pack: shelf packing needs the sizes
    struct Raster { uint32_t cp; std::vector<uint8_t> px; int w, h; float bx, by, adv; };
    std::vector<Raster> rasters;
    for (uint32_t rp = 0; rp < range_pairs; ++rp) {
        for (uint32_t cp = ranges[rp * 2]; cp <= ranges[rp * 2 + 1]; ++cp) {
            uint32_t gid = glyph_index(f, cp);
            std::vector<Contour> contours;
            load_contours(f, gid, contours);
            float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
            for (Contour &c : contours)
                for (Point &p : c.pts) {
                    p.x *= f->scale; p.y *= f->scale;
                    minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                    miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
                }
            uint16_t adv_units = f->num_hmetrics
                ? f->r.u16(f->hmtx + (size_t)std::min<uint32_t>(gid, (uint32_t)f->num_hmetrics - 1) * 4) : 0;
            Raster ras{};
            ras.cp = cp;
            ras.adv = adv_units * f->scale;
            if (contours.empty() || maxx <= minx || maxy <= miny) {
                ras.w = ras.h = 0; ras.bx = ras.by = 0;
            } else {
                float ox = std::floor(minx), oy = std::floor(miny);
                ras.w = (int)std::ceil(maxx - ox) + 1;
                ras.h = (int)std::ceil(maxy - oy) + 1;
                ras.bx = ox; ras.by = oy;
                rasterise(contours, ras.w, ras.h, ox, oy, ras.px);
            }
            rasters.push_back(std::move(ras));
        }
    }

    uint32_t atlas_w = 512;
    while (atlas_w < (uint32_t)pixel_height * 4) atlas_w *= 2;
    uint32_t x = 1, y = 1, row_h = 0;
    for (const Raster &ras : rasters) {
        if (x + (uint32_t)ras.w + 1 >= atlas_w) { x = 1; y += row_h + 1; row_h = 0; }
        x += (uint32_t)ras.w + 1;
        row_h = std::max(row_h, (uint32_t)ras.h);
    }
    uint32_t atlas_h = 1;
    while (atlas_h < y + row_h + 1) atlas_h *= 2;

    f->aw = atlas_w; f->ah = atlas_h;
    f->atlas.assign((size_t)atlas_w * atlas_h, 0);
    x = 1; y = 1; row_h = 0;
    for (const Raster &ras : rasters) {
        if (x + (uint32_t)ras.w + 1 >= atlas_w) { x = 1; y += row_h + 1; row_h = 0; }
        for (int gy = 0; gy < ras.h; ++gy)
            for (int gx = 0; gx < ras.w; ++gx)
                f->atlas[(size_t)(y + (uint32_t)gy) * atlas_w + (x + (uint32_t)gx)] = ras.px[(size_t)gy * ras.w + gx];

        dai_glyph g{};
        g.codepoint = ras.cp;
        g.u0 = (float)x / atlas_w;           g.v0 = (float)y / atlas_h;
        g.u1 = (float)(x + ras.w) / atlas_w; g.v1 = (float)(y + ras.h) / atlas_h;
        // y grows downwards on screen, upwards in font space
        g.x0 = ras.bx;            g.y0 = -(ras.by + (float)ras.h);
        g.x1 = ras.bx + ras.w;    g.y1 = -ras.by;
        g.advance = ras.adv;
        f->glyphs[ras.cp] = g;

        x += (uint32_t)ras.w + 1;
        row_h = std::max(row_h, (uint32_t)ras.h);
    }
    return f;
}

void dai_font_free(dai_font *f) { delete f; }

dai_font *dai_font_load_ui(float pixel_height, char *err, size_t err_len) {
    // Every one of these is a font that ships with the operating system, so a
    // program using this needs nothing installed and nothing bundled.
    static const char *candidates[] = {
        // Windows
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/verdana.ttf",
        // Linux
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        // macOS
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
    };

    const char *override_path = std::getenv("DAI_FONT");
    if (override_path && override_path[0]) {
        dai_font *f = dai_font_load(override_path, pixel_height, nullptr, 0, err, err_len);
        if (f) return f;
        // An explicit DAI_FONT that does not work is a mistake worth reporting
        // rather than papering over with a silent fallback.
        if (err && err_len) {
            char detail[128];
            std::snprintf(detail, sizeof(detail), "%s", err);
            std::snprintf(err, err_len, "DAI_FONT=%s could not be loaded: %s",
                          override_path, detail);
        }
        return nullptr;
    }

    for (const char *path : candidates) {
        dai_font *f = dai_font_load(path, pixel_height, nullptr, 0, nullptr, 0);
        if (f) return f;
    }
    if (err && err_len)
        std::snprintf(err, err_len,
                      "no system UI font found - tried Segoe UI, Tahoma, Arial, DejaVu Sans, "
                      "Liberation Sans and FreeSans; set DAI_FONT to a .ttf to override");
    return nullptr;
}

const uint8_t *dai_font_atlas(const dai_font *f, uint32_t *w, uint32_t *h) {
    if (!f) return nullptr;
    if (w) *w = f->aw;
    if (h) *h = f->ah;
    return f->atlas.data();
}

const uint8_t *dai_font_atlas_rgba(dai_font *f, uint32_t *w, uint32_t *h) {
    if (!f) return nullptr;
    if (f->atlas_rgba.size() != f->atlas.size() * 4) {
        f->atlas_rgba.resize(f->atlas.size() * 4);
        for (size_t i = 0; i < f->atlas.size(); ++i) {
            f->atlas_rgba[i*4+0] = 255; f->atlas_rgba[i*4+1] = 255;
            f->atlas_rgba[i*4+2] = 255; f->atlas_rgba[i*4+3] = f->atlas[i];
        }
    }
    if (w) *w = f->aw;
    if (h) *h = f->ah;
    return f->atlas_rgba.data();
}

const dai_glyph *dai_font_glyph(const dai_font *f, uint32_t cp) {
    if (!f) return nullptr;
    auto it = f->glyphs.find(cp);
    if (it != f->glyphs.end()) return &it->second;
    it = f->glyphs.find('?');
    return it != f->glyphs.end() ? &it->second : nullptr;
}

float dai_font_line_height(const dai_font *f) {
    return f ? (f->ascent - f->descent + f->line_gap) * f->scale : 0.0f;
}
float dai_font_ascent(const dai_font *f) { return f ? f->ascent * f->scale : 0.0f; }

float dai_font_measure(const dai_font *f, const char *utf8, uint32_t *count) {
    if (!f || !utf8) return 0.0f;
    float w = 0.0f;
    uint32_t off = 0, n = 0;
    for (;;) {
        uint32_t cp = dai_utf8_next(utf8, &off);
        if (!cp) break;
        const dai_glyph *g = dai_font_glyph(f, cp);
        if (g) { w += g->advance; ++n; }
    }
    if (count) *count = n;
    return w;
}

} // extern "C"
