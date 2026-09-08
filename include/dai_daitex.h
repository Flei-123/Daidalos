/*
 * The procedural texture baker: a `.daitex` node graph in, three maps out.
 *
 * MODULE 3 owns this file, include/dai_daitex_host.inl and
 * src/dai_editor_ui_daitex.inl. Nothing else.
 *
 * The rule from docs/MATERIALS.md is not bent here, it is the reason this
 * exists at all: **node graphs are an authoring tool, the engine consumes
 * maps.** What reaches the renderer is a base colour PNG, an ORM PNG and a
 * normal PNG - exactly what a Blender bake would have handed over. The only
 * thing that changed is where the bake happens, and that is the whole point:
 * Jarvis cannot open Blender, and a wall needs a surface anyway.
 *
 * Why the implementation is IN the header rather than in src/dai_daitex.cpp:
 * `build.sh` and `build_win.sh` are frozen and name every translation unit
 * they compile, one by one, so a new .cpp is a file that never reaches an
 * archive. This is the same way in that dai_gltf_common.hpp and
 * dai_doc_internal.hpp already use - see include/dai_ext.h. Every function is
 * `inline`, so including it from two hosts costs nothing and warns about
 * nothing.
 *
 * DETERMINISM IS THE CONTRACT. Same graph plus same seed = bit identical
 * pixels, on every machine and every run:
 *
 *   * the only source of randomness is an integer hash of the lattice
 *     coordinate and the seed - no clock, no rand(), no static state;
 *   * no threads, so no reduction can be reordered;
 *   * no std::pow, no exp, no log, no trigonometry - libm is free to round
 *     those differently between versions, and one ulp here is a different
 *     PNG. Integer powers are repeated multiplication, the gamma of the
 *     levels node goes through `pow_pos` below (written out, float only),
 *     and the gradients of the Perlin lattice are a constant table rather
 *     than a sin/cos pair. What is left is sqrtf, frexpf and ldexpf, and all
 *     three are exact by IEEE 754;
 *   * the quantisation to 8 bits is one rounded multiply, done once.
 *
 * tests/test_daitex.cpp bakes every graph in the project twice and compares
 * the PNG BYTES, so a float that comes out one ULP different is a failure and
 * not a rounding detail.
 *
 *     daitex::Graph g;
 *     std::string err;
 *     if (!daitex::parse_file("assets/textures/beton.daitex", &g, &err)) ...
 *     daitex::Bake b;
 *     daitex::bake(g, &b);                       // b.base_color/orm/normal, RGBA8
 *     daitex::write_maps(g, b, path, &err);      // the three PNGs beside it
 *
 * The maps land next to the graph under its own stem:
 * `beton.daitex` -> `beton_basecolor.png`, `beton_orm.png`, `beton_normal.png`,
 * which is what a `.daimat` points its map slots at.
 */
#ifndef DAI_DAITEX_H
#define DAI_DAITEX_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* The engine's own JSON DOM rather than a second one. Reached by a relative
 * path on purpose: the examples are compiled with `-Iinclude` and nothing
 * else, so "dai_json.hpp" would find nothing there - a quoted include is
 * resolved against the directory of the file that writes it, which is this
 * one. The symbols come out of dai_json.o, which is already in
 * libdaidalos_vk.a and already on every link line that draws anything. */
#include "../src/dai_json.hpp"

namespace daitex {

/* ---- the graph, as it sits in the file ---------------------------------- */

/* A numeric parameter, kept in FILE ORDER together with the span of the
 * literal it was read from. The span is what lets the inspector write a
 * slider's new value back into the exact place it came from instead of
 * re-serialising a graph it did not write - a re-serialiser is how a comment,
 * a node it does not know and an author's formatting all get lost. */
struct Param {
    std::string key;
    float       value = 0.0f;
    size_t      span_at = 0;      /* offset of the literal in the file text */
    size_t      span_len = 0;
};

struct Stop {                     /* one colour ramp stop                    */
    float t = 0.0f;
    float rgb[3] = { 0, 0, 0 };
};

struct Node {
    std::string id;
    std::string type;             /* noise, gradient, brick, checker, mix,   */
                                  /* levels, blur, colorramp, normal, ao,    */
                                  /* const                                   */
    std::string kind;             /* the type's variant: "perlin", "add", .. */
    std::string in_a, in_b, in_fac;
    std::vector<Param> params;
    std::vector<Stop>  stops;
    float rgb[3] = { 0, 0, 0 };   /* const, when it is a colour              */
    int   has_rgb = 0;
};

/* An output slot is either a node - the usual case - or a constant, so a
 * material that is metal everywhere does not need a node to say so. */
struct Slot {
    std::string node;
    float       value = 0.0f;
    int         is_node = 0;
};

struct Graph {
    std::string name;
    uint32_t    resolution = 256;
    uint32_t    seed = 0;
    float       tiling_m = 1.0f;  /* metres one tile covers - what module 2's */
                                  /* triplanar tiling size is set from        */
    std::vector<Node> nodes;
    Slot base_color, occlusion, roughness, metallic, normal;
    std::string text;             /* the file, verbatim: see Param::span_at  */
};

/* ---- the result --------------------------------------------------------- */

struct Bake {
    uint32_t size = 0;
    std::vector<uint8_t> base_color;   /* RGBA8, sRGB    */
    std::vector<uint8_t> orm;          /* RGBA8, linear: R=AO G=rough B=metal */
    std::vector<uint8_t> normal;       /* RGBA8, linear tangent space         */
};

/* ---- an intermediate image --------------------------------------------- */

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<float> px;             /* 3 floats per pixel, row major      */
    float r(uint32_t x, uint32_t y) const { return px[((size_t)y * w + x) * 3]; }
};

/* ---- determinism: the only randomness in the file ----------------------- */

inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t hash2(int x, int y, uint32_t seed) {
    return hash_u32((uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA6Bu ^
                    hash_u32(seed + 0x27D4EB2Fu));
}

/* [0,1), 24 bits of it - the low byte of the hash is dropped because it is
 * the one the multiply mixes least. */
inline float hash01(int x, int y, uint32_t seed) {
    return (float)(hash2(x, y, seed) >> 8) * (1.0f / 16777216.0f);
}

/* ---- small maths, written out rather than borrowed ---------------------- */

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smooth5(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/* Integer powers by repeated multiplication: std::pow(2.0f, 3) is allowed to
 * differ by an ulp between libm versions, and one ulp here is a different
 * PNG. */
inline float powi(float base, int e) {
    float v = 1.0f;
    for (int i = 0; i < e; ++i) v *= base;
    return v;
}

inline int wrapi(int v, int n) { int m = v % n; return m < 0 ? m + n : m; }

/* x^e for x >= 0, without libm's pow. 2^(e * log2 x), with both halves
 * written out: frexpf and ldexpf only move the exponent (exact), and the two
 * series below are float arithmetic and nothing else. Good to about 1e-4
 * relative, which is a fortieth of one 8 bit code value - the quantisation
 * that follows swallows it, and unlike libm it is the same everywhere. */
inline float log2_f(float x) {
    if (x <= 0.0f) return -126.0f;
    int e = 0;
    float m = std::frexp(x, &e);        /* m in [0.5,1) */
    m *= 2.0f; e -= 1;                  /* m in [1,2)   */
    float t = (m - 1.0f) / (m + 1.0f);  /* |t| <= 1/3   */
    float t2 = t * t;
    /* ln(m) = 2*(t + t^3/3 + t^5/5 + t^7/7 + t^9/9) */
    float ln = 2.0f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (1.0f / 5.0f +
               t2 * (1.0f / 7.0f + t2 * (1.0f / 9.0f)))));
    return (float)e + ln * 1.44269504f; /* 1/ln 2 */
}

inline float exp2_f(float y) {
    if (y < -126.0f) return 0.0f;
    if (y > 126.0f) y = 126.0f;
    float fn = std::floor(y);
    int n = (int)fn;
    float z = (y - fn) * 0.69314718f;   /* ln 2 */
    /* e^z, z in [0, ln2): Taylor to z^7, error < 1e-6 */
    float s = 1.0f + z * (1.0f + z * (1.0f / 2.0f + z * (1.0f / 6.0f +
              z * (1.0f / 24.0f + z * (1.0f / 120.0f + z * (1.0f / 720.0f +
              z * (1.0f / 5040.0f)))))));
    return std::ldexp(s, n);
}

inline float pow_pos(float x, float e) {
    if (x <= 0.0f) return 0.0f;
    if (e == 1.0f) return x;
    if (e == 2.0f) return x * x;
    if (e == 0.5f) return std::sqrt(x);
    return exp2_f(e * log2_f(x));
}

/* ---- the noise lattice -------------------------------------------------- */

/* Periodic over `cells`, so every noise in this file tiles - which is what
 * makes a triplanar wall have no seam at the corner. */
inline float value_noise(float u, float v, int cells, uint32_t seed) {
    if (cells < 1) cells = 1;
    float fx = u * (float)cells, fy = v * (float)cells;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float tx = smooth5(fx - (float)ix), ty = smooth5(fy - (float)iy);
    int x0 = wrapi(ix, cells), y0 = wrapi(iy, cells);
    int x1 = wrapi(ix + 1, cells), y1 = wrapi(iy + 1, cells);
    float a = hash01(x0, y0, seed), b = hash01(x1, y0, seed);
    float c = hash01(x0, y1, seed), d = hash01(x1, y1, seed);
    return lerpf(lerpf(a, b, tx), lerpf(c, d, tx), ty);
}

/* Eight gradients on the unit circle, as a table: sin/cos of a hashed angle
 * is the usual way and is exactly the kind of libm dependency the determinism
 * contract above rules out. */
inline void perlin_grad(uint32_t h, float *gx, float *gy) {
    static const float G[8][2] = {
        {  1.0f,  0.0f }, { -1.0f,  0.0f }, {  0.0f,  1.0f }, {  0.0f, -1.0f },
        {  0.70710678f,  0.70710678f }, { -0.70710678f,  0.70710678f },
        {  0.70710678f, -0.70710678f }, { -0.70710678f, -0.70710678f },
    };
    const float *g = G[h & 7u];
    *gx = g[0]; *gy = g[1];
}

inline float perlin_noise(float u, float v, int cells, uint32_t seed) {
    if (cells < 1) cells = 1;
    float fx = u * (float)cells, fy = v * (float)cells;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float dx = fx - (float)ix, dy = fy - (float)iy;
    float tx = smooth5(dx), ty = smooth5(dy);
    float dots[4];
    for (int c = 0; c < 4; ++c) {
        int ox = c & 1, oy = (c >> 1) & 1;
        float gx, gy;
        perlin_grad(hash2(wrapi(ix + ox, cells), wrapi(iy + oy, cells), seed), &gx, &gy);
        dots[c] = gx * (dx - (float)ox) + gy * (dy - (float)oy);
    }
    float n = lerpf(lerpf(dots[0], dots[1], tx), lerpf(dots[2], dots[3], tx), ty);
    return clamp01(n * 0.7071068f + 0.5f);      /* the +-1/sqrt2 range, centred */
}

/* F1 Worley, periodic the same way: the neighbour cell is wrapped for the
 * hash and NOT for the distance, which is what keeps the seam invisible. */
inline float worley_noise(float u, float v, int cells, uint32_t seed) {
    if (cells < 1) cells = 1;
    float fx = u * (float)cells, fy = v * (float)cells;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
    float best = 4.0f;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            int cx = ix + ox, cy = iy + oy;
            uint32_t h = hash2(wrapi(cx, cells), wrapi(cy, cells), seed);
            float px = (float)cx + (float)(h >> 8) * (1.0f / 16777216.0f);
            float py = (float)cy + (float)(hash_u32(h) >> 8) * (1.0f / 16777216.0f);
            float ddx = px - fx, ddy = py - fy;
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 < best) best = d2;
        }
    }
    return clamp01(std::sqrt(best));
}

inline float fbm(const char *kind, float u, float v, int cells, int octaves,
                 float gain, float lacunarity, uint32_t seed) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        /* An INTEGER cell count per octave: a fractional frequency does not
         * divide the unit square and the tile stops matching itself. */
        int c = (int)(cells * powi(lacunarity, o) + 0.5f);
        if (c < 1) c = 1;
        float n;
        if (!std::strcmp(kind, "perlin"))      n = perlin_noise(u, v, c, seed + (uint32_t)o * 131u);
        else if (!std::strcmp(kind, "worley")) n = worley_noise(u, v, c, seed + (uint32_t)o * 131u);
        else                                   n = value_noise(u, v, c, seed + (uint32_t)o * 131u);
        sum += n * amp;
        norm += amp;
        amp *= gain;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/* ---- parsing ------------------------------------------------------------ */

inline float param_of(const Node &n, const char *key, float def) {
    for (const Param &p : n.params) if (p.key == key) return p.value;
    return def;
}

/* The slider range a parameter gets in the inspector. A table rather than a
 * per node if-chain, because the inspector, the clamp on load and the
 * documentation then cannot disagree about what "cells" means. */
inline void param_range(const char *type, const char *key, float *lo, float *hi, float *step) {
    *lo = 0.0f; *hi = 1.0f; *step = 0.01f;
    int hit = 0;
    /* The one table, shared with the inspector - which cannot include a
     * header, so the table is a block of statements. See the file itself. */
#define DAI_DAITEX_RANGE(t, k, a, b, s)                                        \
    if (!hit && !std::strcmp(t, type) && !std::strcmp(k, key)) {               \
        *lo = (a); *hi = (b); *step = (s); hit = 1;                            \
    }
#include "dai_daitex_ranges.inl"
#undef DAI_DAITEX_RANGE
}

inline void read_slot(const daijson::Value *v, Slot *s, float def) {
    s->is_node = 0;
    s->value = def;
    s->node.clear();
    if (!v) return;
    if (v->type == daijson::Value::STRING) { s->node = v->str; s->is_node = 1; }
    else if (v->type == daijson::Value::NUMBER) s->value = (float)v->number;
}

/* Where in `text` the number that produced `v` was written. daijson keeps no
 * offsets, and re-serialising the graph to save one slider is how formatting
 * and unknown keys get lost - so the literal is found by walking the file to
 * the key inside the node's own `"params"` block. Cheap: this runs once per
 * file read, not per frame. */
inline void locate_params(const std::string &text, Node *n) {
    /* The node's own object, found by its id: every `"id"` in the file, until
     * the one whose value is this node's. Ids are unique - a graph with two
     * `"wall"` nodes is a graph whose second one is unreachable anyway. */
    size_t at = std::string::npos;
    for (size_t k = text.find("\"id\""); k != std::string::npos; k = text.find("\"id\"", k + 4)) {
        size_t q1 = text.find('"', text.find(':', k) + 1);
        if (q1 == std::string::npos) break;
        size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        if (text.compare(q1 + 1, q2 - q1 - 1, n->id) == 0) { at = k; break; }
    }
    if (at == std::string::npos) return;
    /* ... and only up to where the NEXT node starts, so a key that this node
     * does not have is not found in the one below it. */
    size_t stop = text.find("\"id\"", at + 4);
    if (stop == std::string::npos) stop = text.size();
    size_t pblock = text.find("\"params\"", at);
    if (pblock == std::string::npos || pblock > stop) return;
    size_t pend = text.find('}', pblock);
    if (pend == std::string::npos || pend > stop) return;
    for (Param &p : n->params) {
        std::string k = "\"" + p.key + "\"";
        size_t kp = text.find(k, pblock);
        if (kp == std::string::npos || kp > pend) continue;
        size_t colon = text.find(':', kp);
        if (colon == std::string::npos || colon > pend) continue;
        size_t s = colon + 1;
        while (s < pend && (text[s] == ' ' || text[s] == '\t')) ++s;
        size_t e = s;
        while (e < pend && (text[e] == '-' || text[e] == '+' || text[e] == '.' ||
                            text[e] == 'e' || text[e] == 'E' ||
                            (text[e] >= '0' && text[e] <= '9'))) ++e;
        if (e > s) { p.span_at = s; p.span_len = e - s; }
    }
}

inline bool parse(const char *text, size_t len, Graph *g, std::string *err) {
    if (!text || !g) { if (err) *err = "no text"; return false; }
    daijson::Document doc;
    std::string jerr;
    if (!doc.parse(text, len, &jerr)) { if (err) *err = jerr; return false; }
    const daijson::Value *root = doc.root();
    if (!root || root->type != daijson::Value::OBJECT) {
        if (err) *err = "a .daitex is a JSON object";
        return false;
    }
    if (!root->get("daitex")) { if (err) *err = "missing \"daitex\" version"; return false; }

    *g = Graph();
    g->text.assign(text, len);
    g->name = root->str_at("name", "");
    int res = root->int_at("resolution", 256);
    if (res < 16) res = 16;
    if (res > 2048) res = 2048;
    g->resolution = (uint32_t)res;
    g->seed = (uint32_t)root->int_at("seed", 0);
    g->tiling_m = (float)root->num_at("tiling_m", 1.0);
    if (g->tiling_m <= 0.0f) g->tiling_m = 1.0f;

    const daijson::Value *nodes = root->get("nodes");
    if (!nodes || nodes->type != daijson::Value::ARRAY || nodes->size() == 0) {
        if (err) *err = "a graph with no nodes bakes nothing";
        return false;
    }
    for (size_t i = 0; i < nodes->size(); ++i) {
        const daijson::Value *nv = nodes->at(i);
        if (!nv || nv->type != daijson::Value::OBJECT) continue;
        Node n;
        n.id = nv->str_at("id", "");
        n.type = nv->str_at("type", "");
        n.kind = nv->str_at("kind", "");
        n.in_a = nv->str_at("in", "");
        n.in_b = nv->str_at("in_b", "");
        n.in_fac = nv->str_at("fac", "");
        if (n.id.empty() || n.type.empty()) {
            if (err) *err = "every node needs an \"id\" and a \"type\"";
            return false;
        }
        const daijson::Value *ps = nv->get("params");
        if (ps && ps->type == daijson::Value::OBJECT) {
            for (const daijson::Member &m : ps->members) {
                if (!m.value || m.value->type != daijson::Value::NUMBER) continue;
                Param p;
                p.key = m.key;
                p.value = (float)m.value->number;
                n.params.push_back(p);
            }
        }
        const daijson::Value *rgb = nv->get("rgb");
        if (rgb && rgb->size() == 3) {
            for (int c = 0; c < 3; ++c) n.rgb[c] = (float)rgb->at((size_t)c)->num(0.0);
            n.has_rgb = 1;
        }
        const daijson::Value *st = nv->get("stops");
        if (st && st->type == daijson::Value::ARRAY) {
            for (size_t s = 0; s < st->size(); ++s) {
                const daijson::Value *sv = st->at(s);
                if (!sv) continue;
                Stop stop;
                stop.t = (float)sv->num_at("t", 0.0);
                const daijson::Value *c = sv->get("rgb");
                if (c && c->size() == 3)
                    for (int k = 0; k < 3; ++k) stop.rgb[k] = (float)c->at((size_t)k)->num(0.0);
                stop.t = clamp01(stop.t);
                n.stops.push_back(stop);
            }
        }
        locate_params(g->text, &n);
        g->nodes.push_back(n);
    }

    const daijson::Value *out = root->get("out");
    if (!out || out->type != daijson::Value::OBJECT) {
        if (err) *err = "missing \"out\": which node is base colour, which is normal";
        return false;
    }
    read_slot(out->get("base_color"), &g->base_color, 0.8f);
    read_slot(out->get("occlusion"),  &g->occlusion,  1.0f);
    read_slot(out->get("roughness"),  &g->roughness,  0.5f);
    read_slot(out->get("metallic"),   &g->metallic,   0.0f);
    read_slot(out->get("normal"),     &g->normal,     0.0f);
    return true;
}

inline bool parse(const char *text, Graph *g, std::string *err) {
    return parse(text, text ? std::strlen(text) : 0, g, err);
}

inline bool read_file(const char *path, std::string *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    char buf[4096];
    out->clear();
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}

inline bool parse_file(const char *path, Graph *g, std::string *err) {
    std::string text;
    if (!read_file(path, &text)) {
        if (err) *err = std::string("cannot read ") + (path ? path : "(null)");
        return false;
    }
    return parse(text.c_str(), text.size(), g, err);
}

/* ---- evaluation --------------------------------------------------------- */

namespace detail {

inline void fill_gray(Image &im, float v) {
    for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = v;
}

inline const Image *find(const std::vector<Image> &imgs, const std::vector<Node> &nodes,
                         const std::string &id) {
    if (id.empty()) return nullptr;
    for (size_t i = 0; i < nodes.size() && i < imgs.size(); ++i)
        if (nodes[i].id == id) return &imgs[i];
    return nullptr;
}

inline float mix_op(const char *mode, float a, float b, float f) {
    if (!std::strcmp(mode, "add"))    return a + b * f;
    if (!std::strcmp(mode, "sub"))    return a - b * f;
    if (!std::strcmp(mode, "mul"))    return lerpf(a, a * b, f);
    if (!std::strcmp(mode, "min"))    return lerpf(a, a < b ? a : b, f);
    if (!std::strcmp(mode, "max"))    return lerpf(a, a > b ? a : b, f);
    if (!std::strcmp(mode, "screen")) return lerpf(a, 1.0f - (1.0f - a) * (1.0f - b), f);
    if (!std::strcmp(mode, "overlay")) {
        float o = a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
        return lerpf(a, o, f);
    }
    return lerpf(a, b, f);                       /* "mix" */
}

/* Separable box blur, wrapped - a blur that clamps at the edge is a blur that
 * puts a seam on a tile that had none. */
inline void blur_image(Image &im, int radius, int passes) {
    if (radius < 1 || passes < 1) return;
    if (passes > 4) passes = 4;
    const int W = (int)im.w, H = (int)im.h;
    std::vector<float> tmp(im.px.size());
    float inv = 1.0f / (float)(radius * 2 + 1);
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) {
                    float s = 0.0f;
                    for (int d = -radius; d <= radius; ++d)
                        s += im.px[((size_t)y * W + wrapi(x + d, W)) * 3 + c];
                    tmp[((size_t)y * W + x) * 3 + c] = s * inv;
                }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) {
                    float s = 0.0f;
                    for (int d = -radius; d <= radius; ++d)
                        s += tmp[((size_t)wrapi(y + d, H) * W + x) * 3 + c];
                    im.px[((size_t)y * W + x) * 3 + c] = s * inv;
                }
    }
}

inline void eval_node(const Node &n, const std::vector<Node> &nodes,
                      const std::vector<Image> &done, const Graph &g, Image &out) {
    const int N = (int)out.w;
    const Image *A = find(done, nodes, n.in_a);
    const Image *B = find(done, nodes, n.in_b);
    const Image *F = find(done, nodes, n.in_fac);
    const std::string &t = n.type;

    if (t == "noise") {
        int cells = (int)param_of(n, "cells", 8.0f);
        int oct = (int)param_of(n, "octaves", 1.0f);
        float gain = param_of(n, "gain", 0.5f);
        float lac = param_of(n, "lacunarity", 2.0f);
        uint32_t seed = g.seed + (uint32_t)param_of(n, "seed", 0.0f);
        const char *kind = n.kind.empty() ? "value" : n.kind.c_str();
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float u = ((float)x + 0.5f) / (float)N, v = ((float)y + 0.5f) / (float)N;
                float s = fbm(kind, u, v, cells, oct, gain, lac, seed);
                for (int c = 0; c < 3; ++c) out.px[((size_t)y * N + x) * 3 + c] = s;
            }
        return;
    }
    if (t == "gradient") {
        float from = param_of(n, "from", 0.0f), to = param_of(n, "to", 1.0f);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float u = ((float)x + 0.5f) / (float)N, v = ((float)y + 0.5f) / (float)N;
                float s;
                if (n.kind == "y")           s = v;
                else if (n.kind == "radial") {
                    float dx = u - 0.5f, dy = v - 0.5f;
                    s = clamp01(std::sqrt(dx * dx + dy * dy) * 2.0f);
                } else if (n.kind == "diag") s = clamp01((u + v) * 0.5f);
                else                         s = u;
                s = lerpf(from, to, s);
                for (int c = 0; c < 3; ++c) out.px[((size_t)y * N + x) * 3 + c] = s;
            }
        return;
    }
    if (t == "brick") {
        int rows = (int)param_of(n, "rows", 8.0f), cols = (int)param_of(n, "cols", 4.0f);
        if (rows < 1) rows = 1;
        if (cols < 1) cols = 1;
        float gap = param_of(n, "gap", 0.04f);
        float shift = param_of(n, "shift", 0.5f);
        float bevel = param_of(n, "bevel", 0.05f);
        float variation = param_of(n, "variation", 0.0f);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float u = ((float)x + 0.5f) / (float)N, v = ((float)y + 0.5f) / (float)N;
                float fy = v * (float)rows;
                int ry = (int)std::floor(fy);
                float ty = fy - (float)ry;
                float ux = u + (float)wrapi(ry, 2) * shift;
                float fx = ux * (float)cols;
                int rx = (int)std::floor(fx);
                float tx = fx - (float)rx;
                /* distance to the nearest mortar line, in brick units */
                float dx = tx < 0.5f ? tx : 1.0f - tx;
                float dy = ty < 0.5f ? ty : 1.0f - ty;
                float d = dx < dy ? dx : dy;
                float s;
                if (d <= gap * 0.5f) s = 0.0f;
                else if (d >= gap * 0.5f + bevel) s = 1.0f;
                else s = smooth5((d - gap * 0.5f) / (bevel > 0.0f ? bevel : 1.0f));
                if (variation > 0.0f)
                    s *= 1.0f - variation * hash01(wrapi(rx, cols), wrapi(ry, rows), g.seed + 7u);
                for (int c = 0; c < 3; ++c) out.px[((size_t)y * N + x) * 3 + c] = clamp01(s);
            }
        return;
    }
    if (t == "checker") {
        int rows = (int)param_of(n, "rows", 4.0f), cols = (int)param_of(n, "cols", 4.0f);
        if (rows < 1) rows = 1;
        if (cols < 1) cols = 1;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                int cx = (int)(((float)x + 0.5f) / (float)N * (float)cols);
                int cy = (int)(((float)y + 0.5f) / (float)N * (float)rows);
                float s = ((cx + cy) & 1) ? 1.0f : 0.0f;
                for (int c = 0; c < 3; ++c) out.px[((size_t)y * N + x) * 3 + c] = s;
            }
        return;
    }
    if (t == "mix") {
        float fac = param_of(n, "factor", 0.5f);
        const char *mode = n.kind.empty() ? "mix" : n.kind.c_str();
        for (size_t i = 0; i < out.px.size(); i += 3) {
            float f = F ? F->px[i] : fac;
            for (int c = 0; c < 3; ++c) {
                float a = A ? A->px[i + c] : 0.0f;
                float b = B ? B->px[i + c] : 0.0f;
                out.px[i + c] = clamp01(mix_op(mode, a, b, f));
            }
        }
        return;
    }
    if (t == "levels") {
        float lo = param_of(n, "in_min", 0.0f), hi = param_of(n, "in_max", 1.0f);
        float olo = param_of(n, "out_min", 0.0f), ohi = param_of(n, "out_max", 1.0f);
        float gamma = param_of(n, "gamma", 1.0f);
        if (gamma < 0.01f) gamma = 0.01f;
        float span = hi - lo;
        for (size_t i = 0; i < out.px.size(); ++i) {
            float v = A ? A->px[i] : 0.0f;
            v = span != 0.0f ? (v - lo) / span : 0.0f;
            v = clamp01(v);
            /* The photographer's convention: gamma > 1 lifts the midtones,
             * so the exponent is 1/gamma. pow_pos, not std::pow - see the top
             * of the file. */
            if (gamma != 1.0f) v = pow_pos(v, 1.0f / gamma);
            out.px[i] = clamp01(lerpf(olo, ohi, v));
        }
        return;
    }
    if (t == "blur") {
        if (A) out.px = A->px; else fill_gray(out, 0.0f);
        blur_image(out, (int)param_of(n, "radius", 2.0f), (int)param_of(n, "passes", 2.0f));
        return;
    }
    if (t == "colorramp") {
        std::vector<Stop> stops = n.stops;
        if (stops.empty()) {
            Stop a; a.t = 0.0f;
            Stop b; b.t = 1.0f; b.rgb[0] = b.rgb[1] = b.rgb[2] = 1.0f;
            stops.push_back(a); stops.push_back(b);
        }
        for (size_t i = 0; i < out.px.size(); i += 3) {
            float v = clamp01(A ? A->px[i] : 0.0f);
            const Stop *lo = &stops[0], *hi = &stops[stops.size() - 1];
            for (size_t s = 0; s + 1 < stops.size(); ++s)
                if (v >= stops[s].t && v <= stops[s + 1].t) { lo = &stops[s]; hi = &stops[s + 1]; break; }
            float span = hi->t - lo->t;
            float f = span > 0.0f ? (v - lo->t) / span : 0.0f;
            f = clamp01(f);
            for (int c = 0; c < 3; ++c) out.px[i + c] = lerpf(lo->rgb[c], hi->rgb[c], f);
        }
        return;
    }
    if (t == "normal") {
        /* Sobel on the height, wrapped, +Y up (glTF's convention). A flat
         * height therefore comes out exactly (0.5, 0.5, 1). */
        float strength = param_of(n, "strength", 1.0f);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float h[3][3];
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                        h[j + 1][i + 1] = A ? A->r((uint32_t)wrapi(x + i, N), (uint32_t)wrapi(y + j, N)) : 0.0f;
                float gx = (h[0][2] + 2.0f * h[1][2] + h[2][2]) - (h[0][0] + 2.0f * h[1][0] + h[2][0]);
                float gy = (h[2][0] + 2.0f * h[2][1] + h[2][2]) - (h[0][0] + 2.0f * h[0][1] + h[0][2]);
                float nx = -gx * strength, ny = gy * strength, nz = 1.0f;
                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len <= 0.0f) len = 1.0f;
                size_t i = ((size_t)y * N + x) * 3;
                out.px[i + 0] = clamp01(nx / len * 0.5f + 0.5f);
                out.px[i + 1] = clamp01(ny / len * 0.5f + 0.5f);
                out.px[i + 2] = clamp01(nz / len * 0.5f + 0.5f);
            }
        return;
    }
    if (t == "ao") {
        /* Ambient occlusion from the height: eight directions, four steps
         * each, all of them fixed - a sample pattern that depends on a random
         * number is a texture that depends on a random number. */
        int radius = (int)param_of(n, "radius", 8.0f);
        if (radius < 1) radius = 1;
        float strength = param_of(n, "strength", 1.0f);
        static const int DIR[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float hc = A ? A->r((uint32_t)x, (uint32_t)y) : 0.0f;
                float occ = 0.0f;
                for (int d = 0; d < 8; ++d)
                    for (int s = 1; s <= 4; ++s) {
                        int sx = wrapi(x + DIR[d][0] * radius * s / 4, N);
                        int sy = wrapi(y + DIR[d][1] * radius * s / 4, N);
                        float hs = A ? A->r((uint32_t)sx, (uint32_t)sy) : 0.0f;
                        float diff = hs - hc;
                        if (diff > 0.0f) occ += diff / (float)s;
                    }
                occ /= 32.0f;
                float v = clamp01(1.0f - occ * strength * 8.0f);
                size_t i = ((size_t)y * N + x) * 3;
                out.px[i] = out.px[i + 1] = out.px[i + 2] = v;
            }
        return;
    }
    /* "const", and anything the file names that this build does not know:
     * a flat value beats a failed bake, and the console line the host prints
     * is what tells the author about it. */
    if (n.has_rgb) {
        for (size_t i = 0; i < out.px.size(); i += 3)
            for (int c = 0; c < 3; ++c) out.px[i + c] = n.rgb[c];
    } else {
        fill_gray(out, param_of(n, "value", 0.5f));
    }
}

inline uint8_t quant(float v) {
    float c = clamp01(v) * 255.0f + 0.5f;
    return (uint8_t)(int)c;
}

} // namespace detail

/* Bakes the graph. Returns false only when the graph is empty - a node that
 * this build does not know evaluates to a flat value rather than stopping the
 * bake, because half a wall is more useful than no wall. */
inline bool bake(const Graph &g, Bake *out) {
    if (!out || g.nodes.empty()) return false;
    const uint32_t N = g.resolution;
    out->size = N;

    std::vector<Image> imgs(g.nodes.size());
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        imgs[i].w = imgs[i].h = N;
        imgs[i].px.assign((size_t)N * N * 3, 0.0f);
        /* In file order: a node may only read the ones above it, which is what
         * makes the graph a DAG without a topological sort and what makes the
         * evaluation order the same on every machine. */
        detail::eval_node(g.nodes[i], g.nodes, imgs, g, imgs[i]);
    }

    const Image *bc = detail::find(imgs, g.nodes, g.base_color.node);
    const Image *ao = detail::find(imgs, g.nodes, g.occlusion.node);
    const Image *ro = detail::find(imgs, g.nodes, g.roughness.node);
    const Image *me = detail::find(imgs, g.nodes, g.metallic.node);
    const Image *nm = detail::find(imgs, g.nodes, g.normal.node);

    out->base_color.assign((size_t)N * N * 4, 255);
    out->orm.assign((size_t)N * N * 4, 255);
    out->normal.assign((size_t)N * N * 4, 255);
    for (size_t p = 0; p < (size_t)N * N; ++p) {
        for (int c = 0; c < 3; ++c)
            out->base_color[p * 4 + c] = detail::quant(bc ? bc->px[p * 3 + c] : g.base_color.value);
        out->orm[p * 4 + 0] = detail::quant(ao ? ao->px[p * 3] : g.occlusion.value);
        out->orm[p * 4 + 1] = detail::quant(ro ? ro->px[p * 3] : g.roughness.value);
        out->orm[p * 4 + 2] = detail::quant(me ? me->px[p * 3] : g.metallic.value);
        if (nm) {
            for (int c = 0; c < 3; ++c) out->normal[p * 4 + c] = detail::quant(nm->px[p * 3 + c]);
        } else {
            out->normal[p * 4 + 0] = 128;
            out->normal[p * 4 + 1] = 128;
            out->normal[p * 4 + 2] = 255;
        }
    }
    return true;
}

/* ---- what the bake is worth as one number ------------------------------- */

/* FNV-1a over all three maps. The test compares two bakes byte for byte; this
 * is what the editor and the tools print, so "the same texture" is a claim
 * anyone can check with their eyes on two lines of output. */
inline uint64_t digest(const Bake &b) {
    uint64_t h = 1469598103934665603ull;
    const std::vector<uint8_t> *all[3] = { &b.base_color, &b.orm, &b.normal };
    for (int i = 0; i < 3; ++i)
        for (uint8_t v : *all[i]) { h ^= v; h *= 1099511628211ull; }
    return h;
}

/* ---- the three files ---------------------------------------------------- */

inline std::string map_path(const std::string &daitex_path, const char *suffix) {
    std::string stem = daitex_path;
    size_t dot = stem.find_last_of('.');
    size_t slash = stem.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        stem.resize(dot);
    return stem + suffix;
}

} // namespace daitex

/* The engine's PNG writer, which every host already links (dai_image.o is in
 * libdaidalos_vk.a). Declared rather than re-implemented, exactly the way
 * src/rhi_vulkan.cpp declares it. */
namespace daiimg {
bool write_png_rgb(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
}

namespace daitex {

/* Writes `<stem>_basecolor.png`, `<stem>_orm.png` and `<stem>_normal.png`
 * beside the graph. All three or none: a material that finds two of its three
 * maps looks like a bug in the renderer. */
inline bool write_maps(const Bake &b, const std::string &daitex_path, std::string *err) {
    if (b.size == 0) { if (err) *err = "nothing baked"; return false; }
    struct { const char *suffix; const std::vector<uint8_t> *px; } M[3] = {
        { "_basecolor.png", &b.base_color },
        { "_orm.png",       &b.orm        },
        { "_normal.png",    &b.normal     },
    };
    for (int i = 0; i < 3; ++i) {
        std::string p = map_path(daitex_path, M[i].suffix);
        if (!daiimg::write_png_rgb(p.c_str(), M[i].px->data(), b.size, b.size)) {
            if (err) *err = "cannot write " + p;
            return false;
        }
    }
    return true;
}

/* A `size` x `size` RGBA8 preview of the base colour, for the Project panel
 * and the inspector. Point sampled on purpose: a thumbnail of a noise is
 * meant to show the noise, and a box filter turns every graph in the folder
 * into the same grey square. */
inline void preview(const Bake &b, uint32_t size, std::vector<uint8_t> *out) {
    out->assign((size_t)size * size * 4, 255);
    if (b.size == 0) return;
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            uint32_t sx = x * b.size / size, sy = y * b.size / size;
            const uint8_t *s = &b.base_color[((size_t)sy * b.size + sx) * 4];
            uint8_t *d = &(*out)[((size_t)y * size + x) * 4];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
}

} // namespace daitex

#endif /* DAI_DAITEX_H */
