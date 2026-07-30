#include "dai_meshgen.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace daimesh {
namespace {

constexpr float PI = 3.14159265358979f;

dai_vertex V(float px, float py, float pz, float nx, float ny, float nz,
             float cap = 0.0f, float u = 0.0f, float v = 0.0f) {
    dai_vertex x{};
    x.position = { px, py, pz }; x.normal = { nx, ny, nz };
    x.cap = cap; x.u = u; x.v = v;
    return x;
}

void quad(Mesh &m, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    m.idx.push_back(a); m.idx.push_back(b); m.idx.push_back(c);
    m.idx.push_back(a); m.idx.push_back(c); m.idx.push_back(d);
}

} // namespace

// ---------------------------------------------------------------- box

Mesh box() {
    // n, u, v with u x v == n keeps every face counter clockwise from outside
    const float F[6][9] = {
        {  1, 0, 0,   0, 0,-1,   0, 1, 0 },
        { -1, 0, 0,   0, 0, 1,   0, 1, 0 },
        {  0, 1, 0,   1, 0, 0,   0, 0,-1 },
        {  0,-1, 0,   1, 0, 0,   0, 0, 1 },
        {  0, 0, 1,   1, 0, 0,   0, 1, 0 },
        {  0, 0,-1,  -1, 0, 0,   0, 1, 0 },
    };
    Mesh m;
    for (int f = 0; f < 6; ++f) {
        const float *n = F[f], *u = F[f] + 3, *v = F[f] + 6;
        uint32_t base = (uint32_t)m.verts.size();
        const float su[4] = { -1,  1,  1, -1 };
        const float sv[4] = { -1, -1,  1,  1 };
        const float tu[4] = {  0,  1,  1,  0 };
        const float tv[4] = {  0,  0,  1,  1 };
        // corner = face centre (the normal) + the two in plane axes. Forgetting
        // the face centre collapses every face onto the origin plane and the
        // cube renders as a flat cross - it looks like a plank from the side.
        for (int k = 0; k < 4; ++k)
            m.verts.push_back(V(n[0] + su[k]*u[0] + sv[k]*v[0],
                                n[1] + su[k]*u[1] + sv[k]*v[1],
                                n[2] + su[k]*u[2] + sv[k]*v[2],
                                n[0], n[1], n[2], 0.0f, tu[k], tv[k]));
        quad(m, base, base + 1, base + 2, base + 3);
    }
    return m;
}

// ---------------------------------------------------------------- sphere

Mesh sphere(int segments, int rings) {
    Mesh m;
    for (int i = 0; i <= rings; ++i) {
        float phi = PI * (float)i / (float)rings;          // 0 = north pole
        float y = cosf(phi), sr = sinf(phi);
        for (int j = 0; j <= segments; ++j) {
            float th = 2.0f * PI * (float)j / (float)segments;
            float x = sr * cosf(th), z = sr * sinf(th);
            m.verts.push_back(V(x, y, z, x, y, z, 0.0f,
                                (float)j / (float)segments, (float)i / (float)rings));
        }
    }
    int stride = segments + 1;
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            uint32_t a = (uint32_t)(i * stride + j);
            quad(m, a, a + 1, a + 1 + stride, a + stride);
        }
    return m;
}

// ---------------------------------------------------------------- capsule

// Radius 1. The two hemispheres carry cap = +-1; the vertex stage moves them
// apart by the instance's param, so a single mesh covers every proportion.
Mesh capsule(int segments, int rings) {
    Mesh m;
    int stride = segments + 1;
    auto hemi = [&](float sign) {
        uint32_t base = (uint32_t)m.verts.size();
        for (int i = 0; i <= rings; ++i) {
            float phi = 0.5f * PI * (float)i / (float)rings;     // 0 = pole
            float y = cosf(phi) * sign, sr = sinf(phi);
            for (int j = 0; j <= segments; ++j) {
                float th = 2.0f * PI * (float)j / (float)segments;
                float x = sr * cosf(th), z = sr * sinf(th);
                m.verts.push_back(V(x, y, z, x, y, z, sign));
            }
        }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segments; ++j) {
                uint32_t a = base + (uint32_t)(i * stride + j);
                if (sign > 0) quad(m, a, a + 1, a + 1 + stride, a + stride);
                else          quad(m, a, a + stride, a + 1 + stride, a + 1);
            }
    };
    hemi(+1.0f);
    hemi(-1.0f);

    uint32_t base = (uint32_t)m.verts.size();
    for (int j = 0; j <= segments; ++j) {
        float th = 2.0f * PI * (float)j / (float)segments;
        float x = cosf(th), z = sinf(th);
        m.verts.push_back(V(x, 0, z, x, 0, z, +1.0f));
        m.verts.push_back(V(x, 0, z, x, 0, z, -1.0f));
    }
    for (int j = 0; j < segments; ++j) {
        uint32_t a = base + (uint32_t)j * 2;      // top ring
        quad(m, a, a + 2, a + 3, a + 1);
    }
    return m;
}

// ---------------------------------------------------------------- cylinder

Mesh cylinder(int segments) {
    Mesh m;
    uint32_t base = (uint32_t)m.verts.size();
    for (int j = 0; j <= segments; ++j) {
        float th = 2.0f * PI * (float)j / (float)segments;
        float x = cosf(th), z = sinf(th);
        m.verts.push_back(V(x,  1, z, x, 0, z));
        m.verts.push_back(V(x, -1, z, x, 0, z));
    }
    for (int j = 0; j < segments; ++j) {
        uint32_t a = base + (uint32_t)j * 2;
        quad(m, a, a + 2, a + 3, a + 1);
    }
    for (int top = 0; top < 2; ++top) {
        float y = top ? 1.0f : -1.0f, ny = y;
        uint32_t c = (uint32_t)m.verts.size();
        m.verts.push_back(V(0, y, 0, 0, ny, 0));
        for (int j = 0; j <= segments; ++j) {
            float th = 2.0f * PI * (float)j / (float)segments;
            m.verts.push_back(V(cosf(th), y, sinf(th), 0, ny, 0));
        }
        for (int j = 0; j < segments; ++j) {
            uint32_t a = c + 1 + (uint32_t)j, b = a + 1;
            if (top) { m.idx.push_back(c); m.idx.push_back(b); m.idx.push_back(a); }
            else     { m.idx.push_back(c); m.idx.push_back(a); m.idx.push_back(b); }
        }
    }
    return m;
}

// ---------------------------------------------------------------- cone

Mesh cone(int segments) {
    Mesh m;
    const float slope = 0.4472136f;   // normal y component for a 1:2 cone
    for (int j = 0; j < segments; ++j) {
        float t0 = 2.0f * PI * (float)j / (float)segments;
        float t1 = 2.0f * PI * (float)(j + 1) / (float)segments;
        float tm = 0.5f * (t0 + t1);
        uint32_t a = (uint32_t)m.verts.size();
        m.verts.push_back(V(0, 1, 0, cosf(tm) * 0.894f, slope, sinf(tm) * 0.894f));
        m.verts.push_back(V(cosf(t0), -1, sinf(t0), cosf(t0) * 0.894f, slope, sinf(t0) * 0.894f));
        m.verts.push_back(V(cosf(t1), -1, sinf(t1), cosf(t1) * 0.894f, slope, sinf(t1) * 0.894f));
        m.idx.push_back(a); m.idx.push_back(a + 2); m.idx.push_back(a + 1);
    }
    uint32_t c = (uint32_t)m.verts.size();
    m.verts.push_back(V(0, -1, 0, 0, -1, 0));
    for (int j = 0; j <= segments; ++j) {
        float th = 2.0f * PI * (float)j / (float)segments;
        m.verts.push_back(V(cosf(th), -1, sinf(th), 0, -1, 0));
    }
    for (int j = 0; j < segments; ++j) {
        uint32_t a = c + 1 + (uint32_t)j;
        m.idx.push_back(c); m.idx.push_back(a); m.idx.push_back(a + 1);
    }
    return m;
}

// ---------------------------------------------------------------- plane

Mesh plane() {
    Mesh m;
    m.verts.push_back(V(-1, 0, -1, 0, 1, 0, 0, 0, 0));
    m.verts.push_back(V(-1, 0,  1, 0, 1, 0, 0, 0, 1));
    m.verts.push_back(V( 1, 0,  1, 0, 1, 0, 0, 1, 1));
    m.verts.push_back(V( 1, 0, -1, 0, 1, 0, 0, 1, 0));
    quad(m, 0, 1, 2, 3);
    return m;
}

// ---------------------------------------------------------------- OBJ

bool load_obj(const char *path, Mesh *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<float> P, N;
    Mesh m;
    char line[1024];
    bool have_normals = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            if (std::sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3) { P.push_back(x); P.push_back(y); P.push_back(z); }
        } else if (line[0] == 'v' && line[1] == 'n') {
            float x, y, z;
            if (std::sscanf(line + 3, "%f %f %f", &x, &y, &z) == 3) { N.push_back(x); N.push_back(y); N.push_back(z); have_normals = true; }
        } else if (line[0] == 'f' && line[1] == ' ') {
            int vi[8] = {0}, ni[8] = {0}, k = 0;
            const char *p = line + 1;
            while (*p && k < 8) {
                while (*p == ' ' || *p == '\t') ++p;
                if (*p == '\0' || *p == '\n' || *p == '\r') break;
                int v = std::atoi(p), n = 0;
                const char *s = std::strchr(p, '/');
                const char *sp = std::strpbrk(p, " \t\r\n");
                if (s && (!sp || s < sp)) {
                    if (s[1] == '/') n = std::atoi(s + 2);
                    else {
                        const char *s2 = std::strchr(s + 1, '/');
                        if (s2 && (!sp || s2 < sp)) n = std::atoi(s2 + 1);
                    }
                }
                vi[k] = v; ni[k] = n; ++k;
                if (!sp) break;
                p = sp;
            }
            auto emit = [&](int a, int b, int c) {
                int tri_v[3] = { vi[a], vi[b], vi[c] };
                int tri_n[3] = { ni[a], ni[b], ni[c] };
                uint32_t base = (uint32_t)m.verts.size();
                float pos[3][3];
                for (int t = 0; t < 3; ++t) {
                    int idx = tri_v[t] > 0 ? tri_v[t] - 1 : (int)(P.size() / 3) + tri_v[t];
                    if (idx < 0 || (size_t)(idx * 3 + 2) >= P.size()) return;
                    pos[t][0] = P[idx*3]; pos[t][1] = P[idx*3+1]; pos[t][2] = P[idx*3+2];
                }
                float e1[3] = { pos[1][0]-pos[0][0], pos[1][1]-pos[0][1], pos[1][2]-pos[0][2] };
                float e2[3] = { pos[2][0]-pos[0][0], pos[2][1]-pos[0][1], pos[2][2]-pos[0][2] };
                float fn[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                float len = std::sqrt(fn[0]*fn[0]+fn[1]*fn[1]+fn[2]*fn[2]);
                if (len > 1e-12f) { fn[0]/=len; fn[1]/=len; fn[2]/=len; }
                for (int t = 0; t < 3; ++t) {
                    float nx = fn[0], ny = fn[1], nz = fn[2];
                    if (have_normals && tri_n[t] != 0) {
                        int idx = tri_n[t] > 0 ? tri_n[t] - 1 : (int)(N.size() / 3) + tri_n[t];
                        if (idx >= 0 && (size_t)(idx * 3 + 2) < N.size()) { nx = N[idx*3]; ny = N[idx*3+1]; nz = N[idx*3+2]; }
                    }
                    m.verts.push_back(V(pos[t][0], pos[t][1], pos[t][2], nx, ny, nz));
                }
                m.idx.push_back(base); m.idx.push_back(base + 1); m.idx.push_back(base + 2);
            };
            for (int t = 1; t + 1 < k; ++t) emit(0, t, t + 1);
        }
    }
    std::fclose(f);
    if (m.verts.empty()) return false;
    *out = std::move(m);
    return true;
}

} // namespace daimesh
