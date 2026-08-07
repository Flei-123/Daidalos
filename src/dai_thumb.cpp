// Asset thumbnails, rasterised on the CPU. See include/dai_thumb.h for why
// this is not a second render pass.
//
// The whole thing is: fit the mesh into a square, walk its triangles through a
// z-buffer, shade each one flat, and box-filter the result down. Orthographic,
// because a thumbnail is an icon - it must not change when the model is scaled,
// and perspective would make a long object lean.

#include "dai_thumb.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct V3 { float x, y, z; };

inline V3 sub(V3 a, V3 b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return V3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline V3 norm(V3 a) {
    float l = std::sqrt(dot(a, a));
    if (l < 1e-20f) return V3{ 0, 0, 1 };
    return V3{ a.x / l, a.y / l, a.z / l };
}

// The view. Fixed, and deliberately so: every thumbnail in the browser is the
// same three quarter view from slightly above, which is what makes a column of
// them comparable at a glance. Blender's default user perspective, roughly.
const V3 VIEW_DIR{ -0.55f, -0.42f, -0.72f };   // from the eye INTO the scene
// The key light lives in VIEW space, not in the world: an icon lit from over
// the viewer's shoulder looks the same whichever way the model was modelled,
// and that is the point of a thumbnail. Pointing TOWARDS the light.
const V3 KEY_VIEW{ -0.40f, 0.55f, 0.73f };

// 2x in each direction. Cheap - the frames are 64 px - and the difference
// between a jagged silhouette and a clean one at icon size is the difference
// between "a model" and "some pixels".
const uint32_t SS = 2;

struct Frame {
    uint32_t n = 0;                 // side, in SUPERSAMPLED pixels
    std::vector<float>   depth;     // bigger is nearer; -inf is empty
    std::vector<float>   lum;       // 0..1 shade
    std::vector<uint8_t> hit;
};

// One triangle. `a`/`b`/`c` are in SCREEN space (x, y in pixels, z as depth);
// `nrm` is the face normal in VIEW space and is passed in rather than worked
// out from the screen triangle.
//
// That distinction cost an hour. A normal taken from the projected triangle is
// not the surface normal: x and y have been multiplied by the fit scale and z
// has not, so the cross product leans towards the viewer by whatever that
// scale happens to be. Every face of a cube then came out the same brightness,
// and - worse - the SAME cube at a different size came out a different
// brightness, because the scale is what changes when the model is bigger.
void raster(Frame &f, V3 a, V3 b, V3 c, V3 nrm) {
    // Two sided: a mesh whose winding is inconsistent (half of everything
    // exported from a sculpting tool) would otherwise have black faces, and a
    // black face reads as a hole.
    if (nrm.z < 0.0f) { nrm.x = -nrm.x; nrm.y = -nrm.y; nrm.z = -nrm.z; }
    float lam = dot(nrm, KEY_VIEW);
    if (lam < 0.0f) lam = 0.0f;
    // Ambient keeps the unlit side readable instead of a silhouette, and the
    // rim term stops a sphere from ending in a hard black edge.
    float shade = 0.24f + 0.70f * lam + 0.10f * nrm.z;
    if (shade > 1.0f) shade = 1.0f;
    if (shade < 0.0f) shade = 0.0f;

    float minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    float maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    float miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    float maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    int x0 = (int)std::floor(minx), x1 = (int)std::ceil(maxx);
    int y0 = (int)std::floor(miny), y1 = (int)std::ceil(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)f.n - 1) x1 = (int)f.n - 1;
    if (y1 > (int)f.n - 1) y1 = (int)f.n - 1;
    if (x1 < x0 || y1 < y0) return;

    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-9f) return;         // a degenerate sliver
    float inv = 1.0f / area;

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            float sx = (float)px + 0.5f, sy = (float)py + 0.5f;
            float w0 = ((b.x - a.x) * (sy - a.y) - (b.y - a.y) * (sx - a.x)) * inv;
            float w1 = ((sx - a.x) * (c.y - a.y) - (sy - a.y) * (c.x - a.x)) * inv;
            // w0 is the weight of c, w1 the weight of b, the rest is a.
            float w2 = 1.0f - w0 - w1;
            const float EPS = -1e-5f;
            if (w0 < EPS || w1 < EPS || w2 < EPS) continue;
            float z = a.z * w2 + b.z * w1 + c.z * w0;
            size_t i = (size_t)py * f.n + (size_t)px;
            if (f.hit[i] && z <= f.depth[i]) continue;
            f.depth[i] = z;
            f.lum[i] = shade;
            f.hit[i] = 1;
        }
    }
}

// A unit shape's triangles, for prefabs made of primitives rather than files.
// Small on purpose: this is a 64 px icon, not the scene.
void push_tri(std::vector<float> &v, V3 a, V3 b, V3 c) {
    v.push_back(a.x); v.push_back(a.y); v.push_back(a.z);
    v.push_back(b.x); v.push_back(b.y); v.push_back(b.z);
    v.push_back(c.x); v.push_back(c.y); v.push_back(c.z);
}

void gen_box(std::vector<float> &v) {
    const float h = 0.5f;
    const V3 p[8] = {
        { -h, -h, -h }, { h, -h, -h }, { h, h, -h }, { -h, h, -h },
        { -h, -h,  h }, { h, -h,  h }, { h, h,  h }, { -h, h,  h },
    };
    static const int F[6][4] = {
        { 4, 5, 6, 7 }, { 1, 0, 3, 2 }, { 0, 4, 7, 3 },
        { 5, 1, 2, 6 }, { 3, 7, 6, 2 }, { 0, 1, 5, 4 },
    };
    for (const auto &f : F) {
        push_tri(v, p[f[0]], p[f[1]], p[f[2]]);
        push_tri(v, p[f[0]], p[f[2]], p[f[3]]);
    }
}

// A UV sphere. `y0`/`y1` clamp it into the two caps of a capsule.
void gen_sphere_part(std::vector<float> &v, float radius, float y_offset,
                     int seg, int ring, float t_from, float t_to) {
    for (int i = 0; i < ring; ++i) {
        float t0 = t_from + (t_to - t_from) * (float)i / (float)ring;
        float t1 = t_from + (t_to - t_from) * (float)(i + 1) / (float)ring;
        float p0 = t0 * 3.14159265f, p1 = t1 * 3.14159265f;
        for (int j = 0; j < seg; ++j) {
            float a0 = 6.2831853f * (float)j / (float)seg;
            float a1 = 6.2831853f * (float)(j + 1) / (float)seg;
            auto at = [&](float ph, float az) {
                return V3{ radius * std::sin(ph) * std::cos(az),
                           radius * std::cos(ph) + y_offset,
                           radius * std::sin(ph) * std::sin(az) };
            };
            V3 A = at(p0, a0), B = at(p1, a0), C = at(p1, a1), D = at(p0, a1);
            push_tri(v, A, B, C);
            push_tri(v, A, C, D);
        }
    }
}

void gen_tube(std::vector<float> &v, float radius, float y_lo, float y_hi, int seg,
              bool caps) {
    for (int j = 0; j < seg; ++j) {
        float a0 = 6.2831853f * (float)j / (float)seg;
        float a1 = 6.2831853f * (float)(j + 1) / (float)seg;
        V3 A{ radius * std::cos(a0), y_lo, radius * std::sin(a0) };
        V3 B{ radius * std::cos(a0), y_hi, radius * std::sin(a0) };
        V3 C{ radius * std::cos(a1), y_hi, radius * std::sin(a1) };
        V3 D{ radius * std::cos(a1), y_lo, radius * std::sin(a1) };
        push_tri(v, A, B, C);
        push_tri(v, A, C, D);
        if (caps) {
            push_tri(v, V3{ 0, y_hi, 0 }, B, C);
            push_tri(v, V3{ 0, y_lo, 0 }, D, A);
        }
    }
}

// One unit shape, centred on the origin and one unit across in every
// direction it has one. The transform a caller hands in carries the real
// size, so these never need to know how big anything is.
// The dai_shape values, not a private numbering. DAI_SHAPE_COMPOUND (3) has
// no geometry of its own - its parts are separate nodes - so it draws as a
// box, and DAI_SHAPE_CYLINDER is 4 because it was added at the END of the
// enum on purpose (see daidalos.h: renumbering would turn every saved capsule
// into a compound).
void gen_unit(int shape, std::vector<float> &v) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:
        gen_sphere_part(v, 0.5f, 0.0f, 20, 12, 0.0f, 1.0f);
        break;
    case DAI_SHAPE_CAPSULE:   // a tube with two half spheres, one unit tall
        gen_tube(v, 0.28f, -0.22f, 0.22f, 20, false);
        gen_sphere_part(v, 0.28f,  0.22f, 20, 6, 0.0f, 0.5f);
        gen_sphere_part(v, 0.28f, -0.22f, 20, 6, 0.5f, 1.0f);
        break;
    case DAI_SHAPE_CYLINDER:
        gen_tube(v, 0.4f, -0.5f, 0.5f, 22, true);
        break;
    default:                  // BOX, COMPOUND, anything a newer file invents
        gen_box(v);
        break;
    }
}

} // namespace

extern "C" {

int dai_thumb_render(const dai_thumb_mesh *mesh, uint8_t *rgba, uint32_t size,
                     uint32_t tint_rgb) {
    if (!rgba || size == 0 || size > 1024) return 0;
    std::memset(rgba, 0, (size_t)size * size * 4);
    if (!mesh || !mesh->positions || mesh->vertex_count < 3) return 0;

    const uint32_t idx_count = mesh->indices ? mesh->index_count : mesh->vertex_count;
    if (idx_count < 3) return 0;

    auto vertex = [&](uint32_t i) -> V3 {
        uint32_t vi = mesh->indices ? mesh->indices[i] : i;
        if (vi >= mesh->vertex_count) vi = 0;
        const float *p = mesh->positions + (size_t)vi * 3;
        return V3{ p[0], p[1], p[2] };
    };

    // ---- the basis the model is looked at through -------------------------
    V3 fwd = norm(VIEW_DIR);
    V3 up_hint{ 0, 1, 0 };
    if (std::fabs(dot(fwd, up_hint)) > 0.99f) up_hint = V3{ 0, 0, 1 };
    V3 right = norm(cross(fwd, up_hint));
    V3 up    = norm(cross(right, fwd));

    // ---- fit: centre on the middle of the projected extent ----------------
    // The bounding box in VIEW space, not in world space. A box fitted in
    // world space and then turned leaves the model off centre, which is
    // visible on every single icon.
    float lo_x = 1e30f, hi_x = -1e30f, lo_y = 1e30f, hi_y = -1e30f;
    bool any = false;
    for (uint32_t i = 0; i < mesh->vertex_count; ++i) {
        const float *p = mesh->positions + (size_t)i * 3;
        V3 w{ p[0], p[1], p[2] };
        float x = dot(w, right), y = dot(w, up);
        if (!(x == x) || !(y == y)) continue;       // a NaN in the file
        if (x < lo_x) lo_x = x;
        if (x > hi_x) hi_x = x;
        if (y < lo_y) lo_y = y;
        if (y > hi_y) hi_y = y;
        any = true;
    }
    if (!any) return 0;
    float ex = (hi_x - lo_x) * 0.5f, ey = (hi_y - lo_y) * 0.5f;
    float half = ex > ey ? ex : ey;
    if (half < 1e-9f) return 0;                     // every vertex in one spot
    float cx = (hi_x + lo_x) * 0.5f, cy = (hi_y + lo_y) * 0.5f;

    Frame f;
    f.n = size * SS;
    f.depth.assign((size_t)f.n * f.n, -1e30f);
    f.lum.assign((size_t)f.n * f.n, 0.0f);
    f.hit.assign((size_t)f.n * f.n, 0);

    // 0.88 leaves a margin: a mesh touching the edge of its own icon looks
    // cropped, and the browser draws these next to each other.
    float scale = ((float)f.n * 0.5f * 0.88f) / half;
    float mid = (float)f.n * 0.5f;
    // World -> view. (right, up, -fwd) is an orthonormal RIGHT handed basis,
    // so a normal computed in here is the real surface normal, merely turned.
    auto to_view = [&](V3 w) {
        return V3{ dot(w, right) - cx, dot(w, up) - cy, -dot(w, fwd) };
    };
    // View -> screen. Only x and y are touched: y is flipped because screen y
    // grows downwards, and getting that wrong is the one bug in this file you
    // would notice instantly.
    auto to_screen = [&](V3 v) {
        return V3{ v.x * scale + mid, mid - v.y * scale, v.z };
    };

    for (uint32_t i = 0; i + 2 < idx_count; i += 3) {
        V3 va = to_view(vertex(i)), vb = to_view(vertex(i + 1)), vc = to_view(vertex(i + 2));
        V3 nrm = norm(cross(sub(vb, va), sub(vc, va)));
        raster(f, to_screen(va), to_screen(vb), to_screen(vc), nrm);
    }

    // ---- resolve --------------------------------------------------------
    float tr = 0.80f, tg = 0.80f, tb = 0.84f;
    if (tint_rgb) {
        tr = (float)((tint_rgb >> 16) & 0xFF) / 255.0f;
        tg = (float)((tint_rgb >> 8) & 0xFF) / 255.0f;
        tb = (float)(tint_rgb & 0xFF) / 255.0f;
    }
    int drawn = 0;
    const float inv_ss = 1.0f / (float)(SS * SS);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float lum = 0.0f;
            int cover = 0;
            for (uint32_t sy = 0; sy < SS; ++sy) {
                for (uint32_t sx = 0; sx < SS; ++sx) {
                    size_t i = (size_t)(y * SS + sy) * f.n + (x * SS + sx);
                    if (!f.hit[i]) continue;
                    lum += f.lum[i];
                    ++cover;
                }
            }
            uint8_t *px = rgba + ((size_t)y * size + x) * 4;
            if (!cover) { px[0] = px[1] = px[2] = px[3] = 0; continue; }
            drawn = 1;
            float l = lum / (float)cover;
            float a = (float)cover * inv_ss;
            auto b8 = [](float v) {
                int i = (int)(v * 255.0f + 0.5f);
                return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
            };
            px[0] = b8(tr * l);
            px[1] = b8(tg * l);
            px[2] = b8(tb * l);
            px[3] = b8(a);
        }
    }
    return drawn;
}

int dai_thumb_render_parts(const dai_thumb_part *parts, uint32_t count,
                           uint8_t *rgba, uint32_t size, uint32_t tint_rgb) {
    if (!rgba || size == 0) return 0;
    if (!parts || !count) {
        std::memset(rgba, 0, (size_t)size * size * 4);
        return 0;
    }
    std::vector<float> all;
    std::vector<float> one;
    for (uint32_t i = 0; i < count; ++i) {
        one.clear();
        gen_unit(parts[i].shape, one);
        const float *m = parts[i].xform;
        for (size_t k = 0; k + 2 < one.size(); k += 3) {
            float x = one[k], y = one[k + 1], z = one[k + 2];
            all.push_back(m[0] * x + m[1] * y + m[2]  * z + m[3]);
            all.push_back(m[4] * x + m[5] * y + m[6]  * z + m[7]);
            all.push_back(m[8] * x + m[9] * y + m[10] * z + m[11]);
        }
    }
    dai_thumb_mesh mesh{ all.data(), (uint32_t)(all.size() / 3), nullptr, 0 };
    return dai_thumb_render(&mesh, rgba, size, tint_rgb);
}

int dai_thumb_render_shape(int shape, uint8_t *rgba, uint32_t size, uint32_t tint_rgb) {
    std::vector<float> v;
    gen_unit(shape, v);
    dai_thumb_mesh m{ v.data(), (uint32_t)(v.size() / 3), nullptr, 0 };
    return dai_thumb_render(&m, rgba, size, tint_rgb);
}

} // extern "C"
