// Shared guts of the glTF importer: the JSON document, the buffer plumbing and
// the accessor readers.
//
// This is a header and not a .cpp because two translation units need it and
// they must NOT be linked together. dai_gltf.cpp builds meshes on a renderer;
// dai_gltf_geom.cpp only hands geometry back as plain arrays, so a build tool
// can read a .glb without dragging in Vulkan. Same parser, two consumers, no
// GPU dependency in the tool.
#ifndef DAI_GLTF_COMMON_HPP
#define DAI_GLTF_COMMON_HPP

// glTF 2.0 / GLB importer. See include/dai_gltf.h.
//
// Contains no Vulkan and no Jolt: it only calls the public renderer API, so it
// works unchanged against any backend that implements dai_render.h.
//
// Scope is deliberate. glTF is a delivery format, not an authoring format, and
// the subset that matters for getting Blender art into an engine is:
// meshes with POSITION/NORMAL/TEXCOORD_0, indexed triangles, the
// metallic-roughness material, PNG textures (embedded or beside the file), and
// the node hierarchy. Skins, morph targets, animations, cameras and lights are
// parsed past, not choked on.

#include "dai_gltf.h"
#include "dai_json.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <vector>

namespace daiimg {
bool read_png(const uint8_t *file, size_t size, std::vector<uint8_t> &rgba,
              uint32_t *w, uint32_t *h, char *err, size_t err_len);
}

namespace {

using daijson::Value;

// ---------------------------------------------------------------- math

struct M4 { float m[16]; };   // column major, same convention as glTF

M4 m4_identity() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r; }

// Inverse of a TRS matrix (column major, m[col*4 + row]). Only the affine case
// is handled, which is all glTF node transforms ever are - and it is needed to
// answer "where is this child relative to its parent" after the hierarchy has
// already been flattened to world space.
inline M4 m4_invert_affine(const M4 &m) {
    float a[3][3];
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) a[r][c] = m.m[c * 4 + r];
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
              - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
              + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (det > -1e-12f && det < 1e-12f) return m4_identity();   // degenerate scale
    float inv_det = 1.0f / det;
    float b[3][3];
    b[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inv_det;
    b[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv_det;
    b[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv_det;
    b[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inv_det;
    b[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv_det;
    b[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv_det;
    b[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inv_det;
    b[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv_det;
    b[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv_det;

    const float t[3] = { m.m[12], m.m[13], m.m[14] };
    M4 out = m4_identity();
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) out.m[c * 4 + r] = b[r][c];
    for (int r = 0; r < 3; ++r)
        out.m[12 + r] = -(b[r][0] * t[0] + b[r][1] * t[1] + b[r][2] * t[2]);
    return out;
}

inline M4 m4_mul(const M4 &a, const M4 &b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + i] * b.m[c * 4 + k];
            r.m[c * 4 + i] = s;
        }
    return r;
}

inline M4 m4_trs(const float t[3], const float q[4], const float s[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = m4_identity();
    r.m[0]  = (1 - 2*y*y - 2*z*z) * s[0];
    r.m[1]  = (2*x*y + 2*z*w)     * s[0];
    r.m[2]  = (2*x*z - 2*y*w)     * s[0];
    r.m[4]  = (2*x*y - 2*z*w)     * s[1];
    r.m[5]  = (1 - 2*x*x - 2*z*z) * s[1];
    r.m[6]  = (2*y*z + 2*x*w)     * s[1];
    r.m[8]  = (2*x*z + 2*y*w)     * s[2];
    r.m[9]  = (2*y*z - 2*x*w)     * s[2];
    r.m[10] = (1 - 2*x*x - 2*y*y) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}

// Splits a world matrix back into translation / rotation / scale. Shear cannot
// survive this - glTF allows it in theory, Blender does not export it, and an
// instance transform has nowhere to put it. Documented rather than silently wrong.
inline void m4_decompose(const M4 &m, dai_vec3 *t, dai_quat *q, dai_vec3 *s) {
    t->x = m.m[12]; t->y = m.m[13]; t->z = m.m[14];
    float cx = sqrtf(m.m[0]*m.m[0] + m.m[1]*m.m[1] + m.m[2]*m.m[2]);
    float cy = sqrtf(m.m[4]*m.m[4] + m.m[5]*m.m[5] + m.m[6]*m.m[6]);
    float cz = sqrtf(m.m[8]*m.m[8] + m.m[9]*m.m[9] + m.m[10]*m.m[10]);
    // mirrored transforms: fold the flip into one axis
    float det = m.m[0]*(m.m[5]*m.m[10] - m.m[6]*m.m[9])
              - m.m[4]*(m.m[1]*m.m[10] - m.m[2]*m.m[9])
              + m.m[8]*(m.m[1]*m.m[6]  - m.m[2]*m.m[5]);
    if (det < 0) cx = -cx;
    s->x = cx; s->y = cy; s->z = cz;

    float r[9];
    float ix = cx != 0 ? 1.0f / cx : 0.0f, iy = cy != 0 ? 1.0f / cy : 0.0f, iz = cz != 0 ? 1.0f / cz : 0.0f;
    r[0] = m.m[0]*ix; r[1] = m.m[1]*ix; r[2] = m.m[2]*ix;
    r[3] = m.m[4]*iy; r[4] = m.m[5]*iy; r[5] = m.m[6]*iy;
    r[6] = m.m[8]*iz; r[7] = m.m[9]*iz; r[8] = m.m[10]*iz;

    float trace = r[0] + r[4] + r[8];
    if (trace > 0) {
        float k = sqrtf(trace + 1.0f) * 2.0f;
        q->w = 0.25f * k;
        q->x = (r[5] - r[7]) / k;
        q->y = (r[6] - r[2]) / k;
        q->z = (r[1] - r[3]) / k;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float k = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2.0f;
        q->w = (r[5] - r[7]) / k; q->x = 0.25f * k;
        q->y = (r[3] + r[1]) / k; q->z = (r[6] + r[2]) / k;
    } else if (r[4] > r[8]) {
        float k = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2.0f;
        q->w = (r[6] - r[2]) / k; q->x = (r[3] + r[1]) / k;
        q->y = 0.25f * k;         q->z = (r[7] + r[5]) / k;
    } else {
        float k = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2.0f;
        q->w = (r[1] - r[3]) / k; q->x = (r[6] + r[2]) / k;
        q->y = (r[7] + r[5]) / k; q->z = 0.25f * k;
    }
    float len = sqrtf(q->x*q->x + q->y*q->y + q->z*q->z + q->w*q->w);
    if (len > 1e-8f) { q->x /= len; q->y /= len; q->z /= len; q->w /= len; }
    else { *q = { 0, 0, 0, 1 }; }
}

// ---------------------------------------------------------------- base64

inline bool base64_decode(const char *s, size_t len, std::vector<uint8_t> &out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        if (c == '=') return -2;
        return -1;
    };
    int buf = 0, bits = 0;
    for (size_t i = 0; i < len; ++i) {
        int v = val(s[i]);
        if (v == -1) continue;       // whitespace and newlines
        if (v == -2) break;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((buf >> bits) & 0xFF)); }
    }
    return true;
}

inline bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    bool ok = n == 0 || std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

inline std::string dir_of(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// percent-decoding: Blender writes "Grid%20copy.png" style URIs
inline std::string uri_decode(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
            out += (char)((hex(in[i+1]) << 4) | hex(in[i+2]));
            i += 2;
        } else out += in[i];
    }
    return out;
}

// ---------------------------------------------------------------- accessors

struct Loader {
    dai_renderer *r = nullptr;
    daijson::Document doc;
    const Value *root = nullptr;
    std::string base_dir;
    // Where a .bin buffer or an external PNG comes from. Without a callback it
    // is base_dir on disk; with one it is whatever the host mounts - which is
    // how a .gltf inside a pack file finds its own sidecars.
    dai_gltf_read_fn sidecar = nullptr;
    void            *sidecar_user = nullptr;

    bool read_ref(const std::string &uri, std::vector<uint8_t> &out) {
        if (sidecar) {
            const void *b = nullptr; size_t n = 0;
            if (!sidecar(uri.c_str(), &b, &n, sidecar_user)) return false;
            const uint8_t *p = (const uint8_t *)b;
            out.assign(p, p + n);
            return true;
        }
        return read_file(base_dir + "/" + uri, out);
    }
    std::vector<std::vector<uint8_t>> buffers;
    const uint8_t *glb_bin = nullptr;
    size_t glb_bin_size = 0;
    char *err = nullptr;
    size_t err_len = 0;

    bool fail(const char *fmt, ...) {
        if (err && err_len) {
            va_list ap; va_start(ap, fmt);
            vsnprintf(err, err_len, fmt, ap);
            va_end(ap);
        }
        return false;
    }

    const uint8_t *view_data(int view_index, size_t *out_size, size_t *out_stride) {
        const Value *views = root->get("bufferViews");
        const Value *v = views ? views->at((size_t)view_index) : nullptr;
        if (!v) return nullptr;
        int buf = v->int_at("buffer", -1);
        size_t off = (size_t)v->num_at("byteOffset", 0);
        size_t len = (size_t)v->num_at("byteLength", 0);
        if (out_stride) *out_stride = (size_t)v->num_at("byteStride", 0);
        if (buf < 0 || (size_t)buf >= buffers.size()) return nullptr;
        const std::vector<uint8_t> &b = buffers[(size_t)buf];
        if (off + len > b.size()) return nullptr;
        if (out_size) *out_size = len;
        return b.data() + off;
    }

    // Reads any accessor as floats, `comps` components per element.
    bool read_accessor_float(int index, int comps, std::vector<float> &out, size_t *count_out) {
        const Value *accs = root->get("accessors");
        const Value *a = accs ? accs->at((size_t)index) : nullptr;
        if (!a) return fail("accessor %d missing", index);
        size_t count = (size_t)a->num_at("count", 0);
        int ctype = a->int_at("componentType", 5126);
        std::string type = a->str_at("type", "SCALAR");
        bool normalized = false;
        if (const Value *n = a->get("normalized")) normalized = n->boolean;
        int type_comps = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 :
                         type == "VEC4" ? 4 : type == "MAT2" ? 4 : type == "MAT3" ? 9 :
                         type == "MAT4" ? 16 : 0;
        // MAT4 matters: inverse bind matrices are stored that way, and treating
        // them as unsupported silently leaves every joint at identity - the mesh
        // then renders in bind pose no matter what the animation does
        if (!type_comps) return fail("accessor type %s not supported", type.c_str());

        int view = a->int_at("bufferView", -1);
        out.assign(count * (size_t)comps, 0.0f);
        if (count_out) *count_out = count;
        if (view < 0) return true;      // sparse-free zero filled accessor

        size_t size = 0, stride = 0;
        const uint8_t *base = view_data(view, &size, &stride);
        if (!base) return fail("bufferView %d unreadable", view);
        base += (size_t)a->num_at("byteOffset", 0);

        size_t elem = 0;
        switch (ctype) {
        case 5120: case 5121: elem = 1; break;
        case 5122: case 5123: elem = 2; break;
        case 5125: case 5126: elem = 4; break;
        default: return fail("componentType %d not supported", ctype);
        }
        size_t packed = elem * (size_t)type_comps;
        if (!stride) stride = packed;

        for (size_t i = 0; i < count; ++i) {
            const uint8_t *p = base + i * stride;
            for (int c = 0; c < comps; ++c) {
                float v = 0.0f;
                if (c < type_comps) {
                    const uint8_t *q = p + (size_t)c * elem;
                    switch (ctype) {
                    case 5126: { float f; std::memcpy(&f, q, 4); v = f; break; }
                    case 5125: { uint32_t u; std::memcpy(&u, q, 4); v = (float)u; break; }
                    case 5123: { uint16_t u; std::memcpy(&u, q, 2); v = normalized ? u / 65535.0f : (float)u; break; }
                    case 5122: { int16_t s; std::memcpy(&s, q, 2); v = normalized ? fmaxf(s / 32767.0f, -1.0f) : (float)s; break; }
                    case 5121: { uint8_t u = *q; v = normalized ? u / 255.0f : (float)u; break; }
                    case 5120: { int8_t s = (int8_t)*q; v = normalized ? fmaxf(s / 127.0f, -1.0f) : (float)s; break; }
                    }
                }
                out[i * (size_t)comps + (size_t)c] = v;
            }
        }
        return true;
    }

    bool read_indices(int index, std::vector<uint32_t> &out) {
        const Value *accs = root->get("accessors");
        const Value *a = accs ? accs->at((size_t)index) : nullptr;
        if (!a) return fail("index accessor %d missing", index);
        size_t count = (size_t)a->num_at("count", 0);
        int ctype = a->int_at("componentType", 5125);
        int view = a->int_at("bufferView", -1);
        size_t size = 0, stride = 0;
        const uint8_t *base = view_data(view, &size, &stride);
        if (!base) return fail("index bufferView %d unreadable", view);
        base += (size_t)a->num_at("byteOffset", 0);
        size_t elem = ctype == 5125 ? 4 : ctype == 5123 ? 2 : 1;
        if (!stride) stride = elem;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *p = base + i * stride;
            uint32_t v = 0;
            if (elem == 4) std::memcpy(&v, p, 4);
            else if (elem == 2) { uint16_t u; std::memcpy(&u, p, 2); v = u; }
            else v = *p;
            out[i] = v;
        }
        return true;
    }
};

} // namespace

#endif // DAI_GLTF_COMMON_HPP
