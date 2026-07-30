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

M4 m4_mul(const M4 &a, const M4 &b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + i] * b.m[c * 4 + k];
            r.m[c * 4 + i] = s;
        }
    return r;
}

M4 m4_trs(const float t[3], const float q[4], const float s[3]) {
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
void m4_decompose(const M4 &m, dai_vec3 *t, dai_quat *q, dai_vec3 *s) {
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

bool base64_decode(const char *s, size_t len, std::vector<uint8_t> &out) {
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

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    bool ok = n == 0 || std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

std::string dir_of(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// percent-decoding: Blender writes "Grid%20copy.png" style URIs
std::string uri_decode(const std::string &in) {
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
        int type_comps = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
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

// ---------------------------------------------------------------- model

struct dai_model {
    std::vector<dai_model_node> nodes;
    dai_model_info info{};
};

extern "C" {

dai_model *dai_gltf_load(dai_renderer *r, const char *path, char *err, size_t err_len) {
    auto bail = [&](const char *m) -> dai_model * {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return nullptr;
    };
    if (!r || !path) return bail("no renderer or path");

    std::vector<uint8_t> file;
    if (!read_file(path, file)) return bail("cannot read file");

    Loader ld;
    ld.r = r; ld.err = err; ld.err_len = err_len;
    ld.base_dir = dir_of(path);

    const char *json_text = nullptr;
    size_t json_len = 0;

    if (file.size() > 12 && !std::memcmp(file.data(), "glTF", 4)) {
        // GLB container: 12 byte header, then chunks (length, type, payload)
        uint32_t total; std::memcpy(&total, file.data() + 8, 4);
        size_t pos = 12;
        while (pos + 8 <= file.size()) {
            uint32_t clen, ctype;
            std::memcpy(&clen, file.data() + pos, 4);
            std::memcpy(&ctype, file.data() + pos + 4, 4);
            const uint8_t *payload = file.data() + pos + 8;
            if (pos + 8 + clen > file.size()) return bail("truncated GLB chunk");
            if (ctype == 0x4E4F534A) { json_text = (const char *)payload; json_len = clen; }
            else if (ctype == 0x004E4942) { ld.glb_bin = payload; ld.glb_bin_size = clen; }
            pos += 8 + clen + ((4 - (clen & 3)) & 3);
        }
        if (!json_text) return bail("GLB without a JSON chunk");
    } else {
        json_text = (const char *)file.data();
        json_len = file.size();
    }

    std::string jerr;
    if (!ld.doc.parse(json_text, json_len, &jerr)) {
        if (err && err_len) std::snprintf(err, err_len, "%s", jerr.c_str());
        return nullptr;
    }
    ld.root = ld.doc.root();
    if (!ld.root || ld.root->type != Value::OBJECT) return bail("glTF root is not an object");

    // ---- buffers
    if (const Value *bufs = ld.root->get("buffers")) {
        for (size_t i = 0; i < bufs->size(); ++i) {
            const Value *b = bufs->at(i);
            std::vector<uint8_t> data;
            const char *uri = b->str_at("uri", "");
            if (!uri[0]) {
                if (!ld.glb_bin) return bail("buffer without uri and no GLB binary chunk");
                data.assign(ld.glb_bin, ld.glb_bin + ld.glb_bin_size);
            } else if (!std::strncmp(uri, "data:", 5)) {
                const char *comma = std::strchr(uri, ',');
                if (!comma) return bail("malformed data uri");
                base64_decode(comma + 1, std::strlen(comma + 1), data);
            } else {
                if (!read_file(ld.base_dir + "/" + uri_decode(uri), data))
                    return bail("cannot read external buffer");
            }
            ld.buffers.push_back(std::move(data));
        }
    }

    // ---- textures, cached per (image, colour space): the same image can be
    //      a base colour map (sRGB) and an ORM map (linear) in one file
    std::unordered_map<uint64_t, dai_texture> tex_cache;
    auto load_texture = [&](int tex_index, bool srgb) -> dai_texture {
        if (tex_index < 0) return 0;
        const Value *texs = ld.root->get("textures");
        const Value *t = texs ? texs->at((size_t)tex_index) : nullptr;
        if (!t) return 0;
        int img_index = t->int_at("source", -1);
        if (img_index < 0) return 0;
        uint64_t key = ((uint64_t)img_index << 1) | (srgb ? 1u : 0u);
        auto it = tex_cache.find(key);
        if (it != tex_cache.end()) return it->second;

        const Value *imgs = ld.root->get("images");
        const Value *im = imgs ? imgs->at((size_t)img_index) : nullptr;
        if (!im) return 0;

        std::vector<uint8_t> bytes;
        const char *uri = im->str_at("uri", "");
        if (uri[0] && std::strncmp(uri, "data:", 5)) {
            if (!read_file(ld.base_dir + "/" + uri_decode(uri), bytes)) return 0;
        } else if (uri[0]) {
            const char *comma = std::strchr(uri, ',');
            if (!comma) return 0;
            base64_decode(comma + 1, std::strlen(comma + 1), bytes);
        } else {
            int view = im->int_at("bufferView", -1);
            size_t size = 0;
            const uint8_t *p = ld.view_data(view, &size, nullptr);
            if (!p) return 0;
            bytes.assign(p, p + size);
        }

        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        char terr[128] = {0};
        if (!daiimg::read_png(bytes.data(), bytes.size(), rgba, &w, &h, terr, sizeof(terr))) {
            // JPEG and KTX2 are legal in glTF but not decoded here; a white
            // texture keeps the material usable instead of failing the load
            tex_cache[key] = 0;
            return 0;
        }
        dai_texture tex = dai_render_texture_create(r, rgba.data(), w, h, srgb ? 1 : 0);
        tex_cache[key] = tex;
        return tex;
    };

    // ---- materials
    std::vector<dai_material> materials;
    if (const Value *mats = ld.root->get("materials")) {
        for (size_t i = 0; i < mats->size(); ++i) {
            const Value *m = mats->at(i);
            dai_material_desc d = dai_material_desc_default();
            std::string name = m->str_at("name", "");
            if (!name.empty()) d.name = name.c_str();

            if (const Value *pbr = m->get("pbrMetallicRoughness")) {
                if (const Value *bc = pbr->get("baseColorFactor")) {
                    d.base_color = { (float)bc->at(0)->num(1), (float)bc->at(1)->num(1), (float)bc->at(2)->num(1) };
                }
                d.metallic = (float)pbr->num_at("metallicFactor", 1.0);
                d.roughness = (float)pbr->num_at("roughnessFactor", 1.0);
                if (const Value *t = pbr->get("baseColorTexture"))
                    d.base_color_tex = load_texture(t->int_at("index", -1), true);
                if (const Value *t = pbr->get("metallicRoughnessTexture"))
                    d.orm_tex = load_texture(t->int_at("index", -1), false);
            }
            if (const Value *t = m->get("normalTexture")) {
                d.normal_tex = load_texture(t->int_at("index", -1), false);
                d.normal_strength = (float)t->num_at("scale", 1.0);
            }
            if (const Value *t = m->get("occlusionTexture")) {
                dai_texture occ = load_texture(t->int_at("index", -1), false);
                if (!d.orm_tex) d.orm_tex = occ;           // glTF packs AO in R of the same map
                d.occlusion = (float)t->num_at("strength", 1.0);
            }
            if (const Value *e = m->get("emissiveFactor"))
                d.emissive = { (float)e->at(0)->num(0), (float)e->at(1)->num(0), (float)e->at(2)->num(0) };
            if (const Value *t = m->get("emissiveTexture"))
                d.emissive_tex = load_texture(t->int_at("index", -1), true);
            if (!std::strcmp(m->str_at("alphaMode", "OPAQUE"), "MASK"))
                d.alpha_cutoff = (float)m->num_at("alphaCutoff", 0.5);

            materials.push_back(dai_render_material_create(r, &d));
        }
    }

    // ---- meshes: one renderer mesh per primitive
    struct Prim { dai_mesh mesh; dai_material material; uint32_t tris; uint32_t verts; };
    std::vector<std::vector<Prim>> mesh_prims;
    uint32_t total_tris = 0, total_verts = 0;
    if (const Value *meshes = ld.root->get("meshes")) {
        for (size_t i = 0; i < meshes->size(); ++i) {
            const Value *mesh = meshes->at(i);
            std::vector<Prim> prims;
            const Value *plist = mesh->get("primitives");
            for (size_t p = 0; plist && p < plist->size(); ++p) {
                const Value *prim = plist->at(p);
                if (prim->int_at("mode", 4) != 4) continue;          // triangles only
                const Value *attrs = prim->get("attributes");
                if (!attrs) continue;
                int a_pos = attrs->int_at("POSITION", -1);
                int a_nrm = attrs->int_at("NORMAL", -1);
                int a_uv  = attrs->int_at("TEXCOORD_0", -1);
                if (a_pos < 0) continue;

                std::vector<float> pos, nrm, uv;
                size_t vcount = 0;
                if (!ld.read_accessor_float(a_pos, 3, pos, &vcount)) return nullptr;
                if (a_nrm >= 0) ld.read_accessor_float(a_nrm, 3, nrm, nullptr);
                if (a_uv  >= 0) ld.read_accessor_float(a_uv,  2, uv,  nullptr);

                std::vector<uint32_t> idx;
                int a_idx = prim->int_at("indices", -1);
                if (a_idx >= 0) { if (!ld.read_indices(a_idx, idx)) return nullptr; }
                else { idx.resize(vcount); for (size_t k = 0; k < vcount; ++k) idx[k] = (uint32_t)k; }

                std::vector<dai_vertex> verts(vcount);
                for (size_t v = 0; v < vcount; ++v) {
                    verts[v].position = { pos[v*3], pos[v*3+1], pos[v*3+2] };
                    verts[v].normal = nrm.size() ? dai_vec3{ nrm[v*3], nrm[v*3+1], nrm[v*3+2] } : dai_vec3{ 0, 1, 0 };
                    verts[v].cap = 0.0f;
                    verts[v].u = uv.size() ? uv[v*2] : 0.0f;
                    verts[v].v = uv.size() ? uv[v*2+1] : 0.0f;
                }
                // no normals in the file: flat shade from the triangles
                if (nrm.empty()) {
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        dai_vertex &A = verts[idx[t]], &B = verts[idx[t+1]], &C = verts[idx[t+2]];
                        float e1[3] = { B.position.x-A.position.x, B.position.y-A.position.y, B.position.z-A.position.z };
                        float e2[3] = { C.position.x-A.position.x, C.position.y-A.position.y, C.position.z-A.position.z };
                        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                        float len = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                        if (len > 1e-12f) { n[0]/=len; n[1]/=len; n[2]/=len; }
                        A.normal = B.normal = C.normal = { n[0], n[1], n[2] };
                    }
                }

                Prim pr{};
                pr.mesh = dai_render_mesh_create(r, verts.data(), (uint32_t)verts.size(), idx.data(), (uint32_t)idx.size());
                int mat = prim->int_at("material", -1);
                pr.material = (mat >= 0 && (size_t)mat < materials.size()) ? materials[(size_t)mat] : 0;
                pr.tris = (uint32_t)(idx.size() / 3);
                pr.verts = (uint32_t)vcount;
                total_tris += pr.tris;
                total_verts += pr.verts;
                prims.push_back(pr);
            }
            mesh_prims.push_back(std::move(prims));
        }
    }

    // ---- nodes, flattened to world space
    dai_model *model = new dai_model();
    float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };

    const Value *nodes = ld.root->get("nodes");
    std::vector<char> visited(nodes ? nodes->size() : 0, 0);

    struct Walker {
        Loader &ld; const Value *nodes; dai_model *model;
        std::vector<std::vector<Prim>> &mesh_prims;
        float *bmin, *bmax;
        std::vector<char> &visited;

        void walk(int index, const M4 &parent) {
            const Value *n = nodes ? nodes->at((size_t)index) : nullptr;
            if (!n || visited[(size_t)index]) return;
            visited[(size_t)index] = 1;

            M4 local = m4_identity();
            if (const Value *m = n->get("matrix")) {
                for (int i = 0; i < 16; ++i) local.m[i] = (float)m->at((size_t)i)->num(0);
            } else {
                float t[3] = { 0,0,0 }, q[4] = { 0,0,0,1 }, s[3] = { 1,1,1 };
                if (const Value *v = n->get("translation")) for (int i = 0; i < 3; ++i) t[i] = (float)v->at((size_t)i)->num(0);
                if (const Value *v = n->get("rotation")) for (int i = 0; i < 4; ++i) q[i] = (float)v->at((size_t)i)->num(i == 3 ? 1 : 0);
                if (const Value *v = n->get("scale")) for (int i = 0; i < 3; ++i) s[i] = (float)v->at((size_t)i)->num(1);
                local = m4_trs(t, q, s);
            }
            M4 world = m4_mul(parent, local);

            int mesh = n->int_at("mesh", -1);
            if (mesh >= 0 && (size_t)mesh < mesh_prims.size()) {
                for (const Prim &p : mesh_prims[(size_t)mesh]) {
                    dai_model_node mn{};
                    mn.mesh = p.mesh;
                    mn.material = p.material;
                    m4_decompose(world, &mn.position, &mn.rotation, &mn.scale);
                    std::snprintf(mn.name, sizeof(mn.name), "%s", n->str_at("name", ""));
                    model->nodes.push_back(mn);
                    for (int i = 0; i < 3; ++i) {
                        float c = (&mn.position.x)[i], e = fabsf((&mn.scale.x)[i]) * 1.0f;
                        if (c - e < bmin[i]) bmin[i] = c - e;
                        if (c + e > bmax[i]) bmax[i] = c + e;
                    }
                }
            }
            if (const Value *ch = n->get("children"))
                for (size_t i = 0; i < ch->size(); ++i) walk(ch->at(i)->integer(-1), world);
        }
    } walker{ ld, nodes, model, mesh_prims, bmin, bmax, visited };

    M4 ident = m4_identity();
    bool walked_any = false;
    if (const Value *scenes = ld.root->get("scenes")) {
        int si = ld.root->int_at("scene", 0);
        const Value *sc = scenes->at((size_t)(si < 0 ? 0 : si));
        if (sc) {
            if (const Value *list = sc->get("nodes")) {
                for (size_t i = 0; i < list->size(); ++i) walker.walk(list->at(i)->integer(-1), ident);
                walked_any = true;
            }
        }
    }
    if (!walked_any && nodes)                       // no scene: take every root node
        for (size_t i = 0; i < nodes->size(); ++i) walker.walk((int)i, ident);

    model->info.nodes = (uint32_t)model->nodes.size();
    model->info.meshes = 0;
    for (auto &v : mesh_prims) model->info.meshes += (uint32_t)v.size();
    model->info.materials = (uint32_t)materials.size();
    model->info.textures = (uint32_t)tex_cache.size();
    model->info.triangles = total_tris;
    model->info.vertices = total_verts;
    if (model->nodes.empty()) { bmin[0]=bmin[1]=bmin[2]=bmax[0]=bmax[1]=bmax[2]=0; }
    model->info.bounds_min = { bmin[0], bmin[1], bmin[2] };
    model->info.bounds_max = { bmax[0], bmax[1], bmax[2] };
    return model;
}

void dai_model_free(dai_model *m) { delete m; }

dai_model_info dai_model_get_info(const dai_model *m) { return m ? m->info : dai_model_info{}; }
uint32_t dai_model_node_count(const dai_model *m) { return m ? (uint32_t)m->nodes.size() : 0; }

const dai_model_node *dai_model_node_at(const dai_model *m, uint32_t i) {
    return (m && i < m->nodes.size()) ? &m->nodes[i] : nullptr;
}

const dai_model_node *dai_model_find(const dai_model *m, const char *name) {
    if (!m || !name) return nullptr;
    for (const dai_model_node &n : m->nodes) if (!std::strcmp(n.name, name)) return &n;
    return nullptr;
}

uint32_t dai_model_instances(const dai_model *m, dai_render_instance *out, uint32_t max,
                             dai_vec3 offset, dai_quat rot, float scale) {
    if (!m || !out) return 0;
    if (scale == 0.0f) scale = 1.0f;
    auto rotate = [](dai_quat q, dai_vec3 v) {
        dai_vec3 u{ q.x, q.y, q.z };
        dai_vec3 uv{ u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
        dai_vec3 uu{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
        return dai_vec3{ v.x + 2.0f*(q.w*uv.x + uu.x), v.y + 2.0f*(q.w*uv.y + uu.y), v.z + 2.0f*(q.w*uv.z + uu.z) };
    };
    auto qmul = [](dai_quat a, dai_quat b) {
        return dai_quat{ a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                         a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                         a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                         a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
    };
    uint32_t n = 0;
    for (const dai_model_node &node : m->nodes) {
        if (n >= max) break;
        dai_render_instance &o = out[n++];
        o = dai_render_instance_default();
        dai_vec3 p = rotate(rot, dai_vec3{ node.position.x * scale, node.position.y * scale, node.position.z * scale });
        o.position = { p.x + offset.x, p.y + offset.y, p.z + offset.z };
        o.rotation = qmul(rot, node.rotation);
        o.scale = { node.scale.x * scale, node.scale.y * scale, node.scale.z * scale };
        o.mesh = node.mesh;
        o.material = node.material;
        o.color = { 1, 1, 1 };
        o.roughness = 1.0f;
    }
    return n;
}

} // extern "C"
