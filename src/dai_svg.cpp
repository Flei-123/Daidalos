// SVG -> coverage bitmap. See include/dai_svg.h for what is and is not here.
//
// The shape of the file: a tiny XML scanner feeds an element handler, the
// element handler turns geometry attributes into cubic segments in DEVICE
// independent user space, and the rasteriser flattens those at the size that
// was actually asked for. Flattening late is the whole reason to keep an SVG
// around instead of a PNG - the same icon is crisp at 14 px and at 64.

#include "dai_svg.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------ maths

struct Mat {           // 2x3 affine: |a c e|
    float a, b, c, d, e, f;   //         |b d f|
};

Mat mat_identity() { return Mat{ 1, 0, 0, 1, 0, 0 }; }

Mat mat_mul(const Mat &m, const Mat &n) {   // apply n, then m
    return Mat{ m.a * n.a + m.c * n.b,
                m.b * n.a + m.d * n.b,
                m.a * n.c + m.c * n.d,
                m.b * n.c + m.d * n.d,
                m.a * n.e + m.c * n.f + m.e,
                m.b * n.e + m.d * n.f + m.f };
}

void mat_apply(const Mat &m, float x, float y, float *ox, float *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

// How much this matrix scales lengths. Exact for a similarity, and the right
// average for anything else - which is what a stroke width needs.
float mat_scale(const Mat &m) {
    float det = std::fabs(m.a * m.d - m.b * m.c);
    return std::sqrt(det > 0.0f ? det : 0.0f);
}

// ------------------------------------------------------------------ geometry

struct Seg {          // one path segment, always stored as a cubic
    float c1x, c1y, c2x, c2y, x, y;
};

struct SubPath {
    float sx = 0, sy = 0;
    std::vector<Seg> segs;
    bool closed = false;
};

enum { CAP_BUTT = 0, CAP_ROUND, CAP_SQUARE };
enum { JOIN_MITER = 0, JOIN_ROUND, JOIN_BEVEL };

struct Shape {
    std::vector<SubPath> subs;
    bool  fill = true;
    bool  evenodd = false;
    bool  stroke = false;
    float stroke_w = 1.0f;
    int   cap = CAP_BUTT;
    int   join = JOIN_MITER;
    float miter = 4.0f;
};

// ------------------------------------------------------------------ scanning

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// A number scanner that copes with what SVG path data actually contains:
// "1.5.5" is two numbers, "1-2" is two numbers, "1e-3" is one.
struct NumReader {
    const char *p, *end;
    NumReader(const char *s, const char *e) : p(s), end(e) {}

    void skip_sep() {
        while (p < end && (is_ws(*p) || *p == ',')) ++p;
    }
    bool at_end() { skip_sep(); return p >= end; }
    // Peek at the next non separator character without consuming it.
    char peek() { skip_sep(); return p < end ? *p : 0; }

    bool number(float *out) {
        skip_sep();
        const char *s = p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        bool digits = false;
        while (p < end && *p >= '0' && *p <= '9') { ++p; digits = true; }
        if (p < end && *p == '.') {
            ++p;
            while (p < end && *p >= '0' && *p <= '9') { ++p; digits = true; }
        }
        if (!digits) { p = s; return false; }
        if (p < end && (*p == 'e' || *p == 'E')) {
            const char *save = p;
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            bool ed = false;
            while (p < end && *p >= '0' && *p <= '9') { ++p; ed = true; }
            if (!ed) p = save;
        }
        std::string tmp(s, (size_t)(p - s));
        *out = (float)std::atof(tmp.c_str());
        return true;
    }
    // Arc flags are single characters and may be written without separators:
    // "a1 1 0 011 5" is a valid, if hostile, arc.
    bool flag(float *out) {
        skip_sep();
        if (p >= end) return false;
        if (*p == '0' || *p == '1') { *out = (float)(*p - '0'); ++p; return true; }
        return number(out);
    }
};

// ------------------------------------------------------------------ path data

struct PathBuilder {
    std::vector<SubPath> subs;
    float cx = 0, cy = 0;        // current point
    float startx = 0, starty = 0;
    float lastc2x = 0, lastc2y = 0;   // for S / T smoothing
    bool  had_cubic = false, had_quad = false;

    void move_to(float x, float y) {
        subs.push_back(SubPath{});
        subs.back().sx = x; subs.back().sy = y;
        cx = startx = x; cy = starty = y;
        had_cubic = had_quad = false;
    }
    SubPath *cur() {
        if (subs.empty()) move_to(cx, cy);
        return &subs.back();
    }
    void line_to(float x, float y) {
        // A line is a cubic whose controls sit on it. One segment type in the
        // rasteriser instead of two, and flattening a straight cubic costs one
        // segment because the adaptive step measures curvature, not length.
        SubPath *s = cur();
        s->segs.push_back(Seg{ cx + (x - cx) / 3.0f, cy + (y - cy) / 3.0f,
                               cx + (x - cx) * 2.0f / 3.0f, cy + (y - cy) * 2.0f / 3.0f, x, y });
        cx = x; cy = y;
        had_cubic = had_quad = false;
    }
    void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        cur()->segs.push_back(Seg{ c1x, c1y, c2x, c2y, x, y });
        lastc2x = c2x; lastc2y = c2y;
        cx = x; cy = y;
        had_cubic = true; had_quad = false;
    }
    void quad_to(float qx, float qy, float x, float y) {
        float c1x = cx + 2.0f / 3.0f * (qx - cx), c1y = cy + 2.0f / 3.0f * (qy - cy);
        float c2x = x + 2.0f / 3.0f * (qx - x),   c2y = y + 2.0f / 3.0f * (qy - y);
        cur()->segs.push_back(Seg{ c1x, c1y, c2x, c2y, x, y });
        lastc2x = qx; lastc2y = qy;      // T reflects the QUADRATIC control
        cx = x; cy = y;
        had_quad = true; had_cubic = false;
    }
    void close() {
        if (subs.empty()) return;
        subs.back().closed = true;
        cx = startx; cy = starty;
        had_cubic = had_quad = false;
    }
};

// Endpoint parameterisation to centre parameterisation, straight out of the
// SVG spec's implementation notes, then split into <=90 degree pieces. A
// quarter circle as a cubic is off by 0.03% - below a pixel at any icon size.
void arc_to(PathBuilder &pb, float rx, float ry, float rot_deg,
            int large, int sweep, float x2, float y2) {
    float x1 = pb.cx, y1 = pb.cy;
    if (x1 == x2 && y1 == y2) return;
    rx = std::fabs(rx); ry = std::fabs(ry);
    if (rx < 1e-6f || ry < 1e-6f) { pb.line_to(x2, y2); return; }

    float phi = rot_deg * 3.14159265358979f / 180.0f;
    float cp = std::cos(phi), sp = std::sin(phi);
    float dx2 = (x1 - x2) * 0.5f, dy2 = (y1 - y2) * 0.5f;
    float x1p =  cp * dx2 + sp * dy2;
    float y1p = -sp * dx2 + cp * dy2;

    // Scale the radii up when they are too small to reach - the spec insists,
    // and without it the sqrt below goes imaginary and the arc vanishes.
    float lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0f) { float s = std::sqrt(lam); rx *= s; ry *= s; }

    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float co = den > 0.0f ? std::sqrt(std::max(0.0f, num / den)) : 0.0f;
    if (large == sweep) co = -co;
    float cxp =  co * rx * y1p / ry;
    float cyp = -co * ry * x1p / rx;
    float ccx = cp * cxp - sp * cyp + (x1 + x2) * 0.5f;
    float ccy = sp * cxp + cp * cyp + (y1 + y2) * 0.5f;

    auto angle = [](float ux, float uy, float vx, float vy) {
        float dot = ux * vx + uy * vy;
        float len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        float a = std::acos(std::max(-1.0f, std::min(1.0f, len > 0 ? dot / len : 1.0f)));
        return (ux * vy - uy * vx < 0.0f) ? -a : a;
    };
    float th1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float dth = angle((x1p - cxp) / rx, (y1p - cyp) / ry,
                      (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    const float TAU = 6.28318530717959f;
    if (!sweep && dth > 0) dth -= TAU;
    else if (sweep && dth < 0) dth += TAU;

    int pieces = (int)std::ceil(std::fabs(dth) / (TAU / 4.0f));
    if (pieces < 1) pieces = 1;
    if (pieces > 8) pieces = 8;
    float step = dth / (float)pieces;
    float k = 4.0f / 3.0f * std::tan(step * 0.25f);
    for (int i = 0; i < pieces; ++i) {
        float a0 = th1 + step * (float)i, a1 = a0 + step;
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);
        // ellipse point and tangent, rotated back into user space
        auto pt = [&](float ca, float sa, float *px, float *py) {
            float ex = rx * ca, ey = ry * sa;
            *px = ccx + cp * ex - sp * ey;
            *py = ccy + sp * ex + cp * ey;
        };
        auto tg = [&](float ca, float sa, float *tx, float *ty) {
            float ex = -rx * sa, ey = ry * ca;
            *tx = cp * ex - sp * ey;
            *ty = sp * ex + cp * ey;
        };
        float p0x, p0y, p1x, p1y, t0x, t0y, t1x, t1y;
        pt(c0, s0, &p0x, &p0y); pt(c1, s1, &p1x, &p1y);
        tg(c0, s0, &t0x, &t0y); tg(c1, s1, &t1x, &t1y);
        pb.cubic_to(p0x + k * t0x, p0y + k * t0y,
                    p1x - k * t1x, p1y - k * t1y, p1x, p1y);
    }
    pb.cx = x2; pb.cy = y2;
}

void parse_path_data(const char *d, size_t len, PathBuilder &pb) {
    NumReader r(d, d + len);
    char cmd = 0;
    float a[7];
    while (true) {
        char c = r.peek();
        if (!c) break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; ++r.p; }
        else if (!cmd) break;      // numbers before any command: give up
        bool rel = cmd >= 'a' && cmd <= 'z';
        char C = (char)(rel ? cmd - 'a' + 'A' : cmd);
        float ox = rel ? pb.cx : 0.0f, oy = rel ? pb.cy : 0.0f;

        switch (C) {
        case 'M':
            if (!r.number(&a[0]) || !r.number(&a[1])) return;
            pb.move_to(a[0] + ox, a[1] + oy);
            cmd = rel ? 'l' : 'L';   // further pairs after an M are line-tos
            break;
        case 'L':
            if (!r.number(&a[0]) || !r.number(&a[1])) return;
            pb.line_to(a[0] + ox, a[1] + oy);
            break;
        case 'H':
            if (!r.number(&a[0])) return;
            pb.line_to(a[0] + ox, pb.cy);
            break;
        case 'V':
            if (!r.number(&a[0])) return;
            pb.line_to(pb.cx, a[0] + oy);
            break;
        case 'C':
            for (int i = 0; i < 6; ++i) if (!r.number(&a[i])) return;
            pb.cubic_to(a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy);
            break;
        case 'S': {
            for (int i = 0; i < 4; ++i) if (!r.number(&a[i])) return;
            float rx = pb.had_cubic ? 2 * pb.cx - pb.lastc2x : pb.cx;
            float ry = pb.had_cubic ? 2 * pb.cy - pb.lastc2y : pb.cy;
            pb.cubic_to(rx, ry, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
            break;
        }
        case 'Q':
            for (int i = 0; i < 4; ++i) if (!r.number(&a[i])) return;
            pb.quad_to(a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
            break;
        case 'T': {
            for (int i = 0; i < 2; ++i) if (!r.number(&a[i])) return;
            float rx = pb.had_quad ? 2 * pb.cx - pb.lastc2x : pb.cx;
            float ry = pb.had_quad ? 2 * pb.cy - pb.lastc2y : pb.cy;
            pb.quad_to(rx, ry, a[0] + ox, a[1] + oy);
            break;
        }
        case 'A':
            if (!r.number(&a[0]) || !r.number(&a[1]) || !r.number(&a[2])) return;
            if (!r.flag(&a[3]) || !r.flag(&a[4])) return;
            if (!r.number(&a[5]) || !r.number(&a[6])) return;
            arc_to(pb, a[0], a[1], a[2], a[3] != 0, a[4] != 0, a[5] + ox, a[6] + oy);
            break;
        case 'Z':
            pb.close();
            break;
        default:
            return;   // an unknown command means the rest is unreadable
        }
        if (C == 'Z') {
            // nothing follows a close but another command
            char n = r.peek();
            if (n && !((n >= 'A' && n <= 'Z') || (n >= 'a' && n <= 'z'))) return;
        }
    }
}

// ------------------------------------------------------------------ XML

struct Attr { std::string name, value; };

// The presentation state an element inherits from its ancestors.
struct State {
    Mat  xf = mat_identity();
    bool fill = true;
    bool evenodd = false;
    bool stroke = false;
    float stroke_w = 1.0f;
    int  cap = CAP_BUTT, join = JOIN_MITER;
    float miter = 4.0f;
};

Mat parse_transform(const char *s, size_t len) {
    Mat m = mat_identity();
    const char *p = s, *end = s + len;
    while (p < end) {
        while (p < end && (is_ws(*p) || *p == ',')) ++p;
        const char *name = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) ++p;
        size_t nlen = (size_t)(p - name);
        while (p < end && is_ws(*p)) ++p;
        if (p >= end || *p != '(') break;
        ++p;
        const char *args = p;
        while (p < end && *p != ')') ++p;
        NumReader r(args, p);
        if (p < end) ++p;
        float v[6]; int n = 0;
        while (n < 6 && r.number(&v[n])) ++n;

        auto is = [&](const char *w) { return nlen == std::strlen(w) && std::strncmp(name, w, nlen) == 0; };
        Mat t = mat_identity();
        const float D2R = 3.14159265358979f / 180.0f;
        if (is("translate") && n >= 1)      t = Mat{ 1, 0, 0, 1, v[0], n >= 2 ? v[1] : 0.0f };
        else if (is("scale") && n >= 1)     t = Mat{ v[0], 0, 0, n >= 2 ? v[1] : v[0], 0, 0 };
        else if (is("rotate") && n >= 1) {
            float c = std::cos(v[0] * D2R), sn = std::sin(v[0] * D2R);
            Mat rot{ c, sn, -sn, c, 0, 0 };
            if (n >= 3) {
                Mat to{ 1, 0, 0, 1, v[1], v[2] }, back{ 1, 0, 0, 1, -v[1], -v[2] };
                t = mat_mul(mat_mul(to, rot), back);
            } else t = rot;
        }
        else if (is("matrix") && n >= 6)    t = Mat{ v[0], v[1], v[2], v[3], v[4], v[5] };
        else if (is("skewX") && n >= 1)     t = Mat{ 1, 0, std::tan(v[0] * D2R), 1, 0, 0 };
        else if (is("skewY") && n >= 1)     t = Mat{ 1, std::tan(v[0] * D2R), 0, 1, 0, 0 };
        m = mat_mul(m, t);
    }
    return m;
}

bool value_is_none(const std::string &v) {
    return v == "none" || v == "None" || v == "NONE" || v == "transparent";
}

void apply_property(State &st, const std::string &k, const std::string &v) {
    if (k == "fill")                   st.fill = !value_is_none(v);
    else if (k == "stroke")            st.stroke = !value_is_none(v);
    else if (k == "stroke-width")      st.stroke_w = (float)std::atof(v.c_str());
    else if (k == "fill-rule")         st.evenodd = (v == "evenodd");
    else if (k == "stroke-linecap")    st.cap = v == "round" ? CAP_ROUND : v == "square" ? CAP_SQUARE : CAP_BUTT;
    else if (k == "stroke-linejoin")   st.join = v == "round" ? JOIN_ROUND : v == "bevel" ? JOIN_BEVEL : JOIN_MITER;
    else if (k == "stroke-miterlimit") st.miter = (float)std::atof(v.c_str());
    else if (k == "stroke-opacity")    { if (std::atof(v.c_str()) <= 0.0) st.stroke = false; }
    else if (k == "fill-opacity")      { if (std::atof(v.c_str()) <= 0.0) st.fill = false; }
}

// style="fill:none;stroke-width:2" - the other half of how icon exporters
// write presentation attributes, and ignoring it draws solid black blobs.
void apply_style(State &st, const std::string &css) {
    size_t i = 0;
    while (i < css.size()) {
        size_t colon = css.find(':', i);
        if (colon == std::string::npos) break;
        size_t semi = css.find(';', colon);
        if (semi == std::string::npos) semi = css.size();
        std::string k = css.substr(i, colon - i);
        std::string v = css.substr(colon + 1, semi - colon - 1);
        auto trim = [](std::string &s) {
            size_t a = 0, b = s.size();
            while (a < b && is_ws(s[a])) ++a;
            while (b > a && is_ws(s[b - 1])) --b;
            s = s.substr(a, b - a);
        };
        trim(k); trim(v);
        apply_property(st, k, v);
        i = semi + 1;
    }
}

} // namespace

// ------------------------------------------------------------------ document

struct dai_svg {
    std::vector<Shape> shapes;
    float vx = 0, vy = 0, vw = 0, vh = 0;
};

namespace {

const std::string *find_attr(const std::vector<Attr> &as, const char *name) {
    for (const Attr &a : as) if (a.name == name) return &a.value;
    return nullptr;
}

float attr_num(const std::vector<Attr> &as, const char *name, float dflt) {
    const std::string *v = find_attr(as, name);
    return v ? (float)std::atof(v->c_str()) : dflt;
}

void add_shape(dai_svg *doc, const State &st, PathBuilder &pb) {
    if (pb.subs.empty()) return;
    if (!st.fill && !st.stroke) return;
    Shape sh;
    sh.fill = st.fill; sh.evenodd = st.evenodd;
    sh.stroke = st.stroke;
    sh.stroke_w = st.stroke_w * mat_scale(st.xf);
    sh.cap = st.cap; sh.join = st.join; sh.miter = st.miter;
    sh.subs.reserve(pb.subs.size());
    for (SubPath &s : pb.subs) {
        if (s.segs.empty()) {
            // A lone moveto still draws a dot when the cap is round - which is
            // how a dotted icon is written.
            if (!(st.stroke && st.cap == CAP_ROUND)) continue;
        }
        SubPath out;
        mat_apply(st.xf, s.sx, s.sy, &out.sx, &out.sy);
        out.closed = s.closed;
        out.segs.reserve(s.segs.size());
        for (const Seg &g : s.segs) {
            Seg t;
            mat_apply(st.xf, g.c1x, g.c1y, &t.c1x, &t.c1y);
            mat_apply(st.xf, g.c2x, g.c2y, &t.c2x, &t.c2y);
            mat_apply(st.xf, g.x, g.y, &t.x, &t.y);
            out.segs.push_back(t);
        }
        sh.subs.push_back(std::move(out));
    }
    if (!sh.subs.empty()) doc->shapes.push_back(std::move(sh));
}

void element(dai_svg *doc, const std::string &tag, const std::vector<Attr> &as, const State &st) {
    PathBuilder pb;
    if (tag == "path") {
        const std::string *d = find_attr(as, "d");
        if (!d) return;
        parse_path_data(d->c_str(), d->size(), pb);
    } else if (tag == "rect") {
        float x = attr_num(as, "x", 0), y = attr_num(as, "y", 0);
        float w = attr_num(as, "width", 0), h = attr_num(as, "height", 0);
        if (w <= 0 || h <= 0) return;
        float rx = attr_num(as, "rx", -1), ry = attr_num(as, "ry", -1);
        if (rx < 0 && ry < 0) { rx = ry = 0; }
        else if (rx < 0) rx = ry;
        else if (ry < 0) ry = rx;
        rx = std::min(rx, w * 0.5f); ry = std::min(ry, h * 0.5f);
        if (rx <= 0 || ry <= 0) {
            pb.move_to(x, y); pb.line_to(x + w, y); pb.line_to(x + w, y + h);
            pb.line_to(x, y + h); pb.close();
        } else {
            pb.move_to(x + rx, y);
            pb.line_to(x + w - rx, y);
            arc_to(pb, rx, ry, 0, 0, 1, x + w, y + ry);
            pb.line_to(x + w, y + h - ry);
            arc_to(pb, rx, ry, 0, 0, 1, x + w - rx, y + h);
            pb.line_to(x + rx, y + h);
            arc_to(pb, rx, ry, 0, 0, 1, x, y + h - ry);
            pb.line_to(x, y + ry);
            arc_to(pb, rx, ry, 0, 0, 1, x + rx, y);
            pb.close();
        }
    } else if (tag == "circle" || tag == "ellipse") {
        float cx = attr_num(as, "cx", 0), cy = attr_num(as, "cy", 0);
        float rx, ry;
        if (tag == "circle") { rx = ry = attr_num(as, "r", 0); }
        else { rx = attr_num(as, "rx", 0); ry = attr_num(as, "ry", 0); }
        if (rx <= 0 || ry <= 0) return;
        pb.move_to(cx + rx, cy);
        arc_to(pb, rx, ry, 0, 0, 1, cx, cy + ry);
        arc_to(pb, rx, ry, 0, 0, 1, cx - rx, cy);
        arc_to(pb, rx, ry, 0, 0, 1, cx, cy - ry);
        arc_to(pb, rx, ry, 0, 0, 1, cx + rx, cy);
        pb.close();
    } else if (tag == "line") {
        pb.move_to(attr_num(as, "x1", 0), attr_num(as, "y1", 0));
        pb.line_to(attr_num(as, "x2", 0), attr_num(as, "y2", 0));
    } else if (tag == "polyline" || tag == "polygon") {
        const std::string *pts = find_attr(as, "points");
        if (!pts) return;
        NumReader r(pts->c_str(), pts->c_str() + pts->size());
        float x, y; bool first = true;
        while (r.number(&x) && r.number(&y)) {
            if (first) { pb.move_to(x, y); first = false; }
            else pb.line_to(x, y);
        }
        if (tag == "polygon") pb.close();
    } else {
        return;
    }
    add_shape(doc, st, pb);
}

// A scanner, not a parser: it knows tags, attributes, comments and CDATA, and
// nothing else. Icon files do not have namespaces that matter, entities that
// matter, or DTDs at all.
void scan_xml(dai_svg *doc, const char *s, const char *end) {
    std::vector<State> stack;
    stack.push_back(State{});
    bool have_viewbox = false;
    float doc_w = 0, doc_h = 0;

    const char *p = s;
    while (p < end) {
        while (p < end && *p != '<') ++p;
        if (p >= end) break;
        ++p;
        if (p < end && *p == '!') {
            if (end - p > 3 && std::strncmp(p, "!--", 3) == 0) {
                const char *e = p;
                while (e + 2 < end && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) ++e;
                p = (e + 2 < end) ? e + 3 : end;
            } else {
                while (p < end && *p != '>') ++p;
                if (p < end) ++p;
            }
            continue;
        }
        if (p < end && *p == '?') {
            while (p < end && *p != '>') ++p;
            if (p < end) ++p;
            continue;
        }
        bool closing = false;
        if (p < end && *p == '/') { closing = true; ++p; }

        const char *ns = p;
        while (p < end && !is_ws(*p) && *p != '>' && *p != '/') ++p;
        std::string tag(ns, (size_t)(p - ns));
        // strip a namespace prefix: svg:path -> path
        size_t colon = tag.find(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);

        if (closing) {
            while (p < end && *p != '>') ++p;
            if (p < end) ++p;
            if (stack.size() > 1) stack.pop_back();
            continue;
        }

        std::vector<Attr> attrs;
        bool self_closing = false;
        while (p < end) {
            while (p < end && is_ws(*p)) ++p;
            if (p < end && *p == '/') { self_closing = true; ++p; continue; }
            if (p < end && *p == '>') { ++p; break; }
            if (p >= end) break;
            const char *as_ = p;
            while (p < end && *p != '=' && !is_ws(*p) && *p != '>' && *p != '/') ++p;
            std::string an(as_, (size_t)(p - as_));
            while (p < end && is_ws(*p)) ++p;
            std::string av;
            if (p < end && *p == '=') {
                ++p;
                while (p < end && is_ws(*p)) ++p;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    const char *vs = p;
                    while (p < end && *p != q) ++p;
                    av.assign(vs, (size_t)(p - vs));
                    if (p < end) ++p;
                } else {
                    const char *vs = p;
                    while (p < end && !is_ws(*p) && *p != '>' && *p != '/') ++p;
                    av.assign(vs, (size_t)(p - vs));
                }
            }
            size_t ac = an.find(':');
            if (ac != std::string::npos) an = an.substr(ac + 1);
            if (!an.empty()) attrs.push_back(Attr{ an, av });
        }

        // Inherit, then override with this element's own attributes.
        State st = stack.back();
        if (const std::string *t = find_attr(attrs, "transform"))
            st.xf = mat_mul(st.xf, parse_transform(t->c_str(), t->size()));
        for (const Attr &a : attrs) {
            if (a.name == "style") continue;
            apply_property(st, a.name, a.value);
        }
        if (const std::string *style = find_attr(attrs, "style")) apply_style(st, *style);

        if (tag == "svg") {
            if (const std::string *vb = find_attr(attrs, "viewBox")) {
                NumReader r(vb->c_str(), vb->c_str() + vb->size());
                float v[4] = { 0, 0, 0, 0 };
                int n = 0;
                while (n < 4 && r.number(&v[n])) ++n;
                if (n == 4 && v[2] > 0 && v[3] > 0) {
                    doc->vx = v[0]; doc->vy = v[1]; doc->vw = v[2]; doc->vh = v[3];
                    have_viewbox = true;
                }
            }
            doc_w = attr_num(attrs, "width", doc_w);
            doc_h = attr_num(attrs, "height", doc_h);
        } else {
            element(doc, tag, attrs, st);
        }

        if (!self_closing) stack.push_back(st);
    }

    if (!have_viewbox) {
        doc->vx = 0; doc->vy = 0;
        doc->vw = doc_w > 0 ? doc_w : 24.0f;
        doc->vh = doc_h > 0 ? doc_h : 24.0f;
    }
}

// ------------------------------------------------------------------ raster

struct Pt { float x, y; };
struct RasterEdge { float x0, y0, x1, y1; int dir; };

// Flattening a cubic: enough segments that the control polygon's deviation
// from the chord is under a third of a pixel AT THE SIZE BEING DRAWN. This is
// the reason the document keeps curves instead of points.
void flatten_cubic(std::vector<Pt> &out, Pt p0, Pt c1, Pt c2, Pt p1) {
    float d = std::fabs(c1.x - p0.x) + std::fabs(c1.y - p0.y) +
              std::fabs(c2.x - c1.x) + std::fabs(c2.y - c1.y) +
              std::fabs(p1.x - c2.x) + std::fabs(p1.y - c2.y);
    int n = (int)std::ceil(std::sqrt(d * 2.0f));
    if (n < 1) n = 1;
    if (n > 48) n = 48;
    for (int i = 1; i <= n; ++i) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        float w0 = mt * mt * mt, w1 = 3 * mt * mt * t, w2 = 3 * mt * t * t, w3 = t * t * t;
        out.push_back(Pt{ w0 * p0.x + w1 * c1.x + w2 * c2.x + w3 * p1.x,
                          w0 * p0.y + w1 * c1.y + w2 * c2.y + w3 * p1.y });
    }
}

// One flattened subpath in device pixels.
struct Poly { std::vector<Pt> pts; bool closed; };

void flatten_shape(const Shape &sh, const Mat &xf, std::vector<Poly> &out) {
    for (const SubPath &s : sh.subs) {
        Poly poly;
        poly.closed = s.closed;
        Pt cur;
        mat_apply(xf, s.sx, s.sy, &cur.x, &cur.y);
        poly.pts.push_back(cur);
        for (const Seg &g : s.segs) {
            Pt c1, c2, p1;
            mat_apply(xf, g.c1x, g.c1y, &c1.x, &c1.y);
            mat_apply(xf, g.c2x, g.c2y, &c2.x, &c2.y);
            mat_apply(xf, g.x, g.y, &p1.x, &p1.y);
            flatten_cubic(poly.pts, cur, c1, c2, p1);
            cur = p1;
        }
        // Points that repeat produce zero length segments, which produce
        // undefined normals, which produce NaNs in the stroker.
        std::vector<Pt> dedup;
        for (const Pt &p : poly.pts) {
            if (!dedup.empty() &&
                std::fabs(p.x - dedup.back().x) < 1e-5f &&
                std::fabs(p.y - dedup.back().y) < 1e-5f) continue;
            dedup.push_back(p);
        }
        if (poly.closed && dedup.size() > 1 &&
            std::fabs(dedup.front().x - dedup.back().x) < 1e-5f &&
            std::fabs(dedup.front().y - dedup.back().y) < 1e-5f) dedup.pop_back();
        poly.pts = std::move(dedup);
        if (!poly.pts.empty()) out.push_back(std::move(poly));
    }
}

// Every polygon handed to the rasteriser is forced counter clockwise, so
// overlapping pieces of one stroke always ADD winding. Get this wrong and a
// join lands on top of its own segment with the opposite sign and punches a
// hole exactly where the ink should be thickest.
// Edges exactly as given: the direction a subpath was written in is the
// information the non zero rule uses to tell a hole from a solid. A donut is
// an outer ring one way round and an inner ring the other way, and "helpfully"
// making them agree fills the hole in.
void emit_poly_raw(std::vector<RasterEdge> &edges, const std::vector<Pt> &pts) {
    if (pts.size() < 3) return;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Pt &a = pts[i], &b = pts[(i + 1) % pts.size()];
        if (a.y == b.y) continue;
        edges.push_back(RasterEdge{ a.x, a.y, b.x, b.y, a.y < b.y ? 1 : -1 });
    }
}

void emit_poly(std::vector<RasterEdge> &edges, std::vector<Pt> &pts) {
    if (pts.size() < 3) return;
    double area = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Pt &a = pts[i], &b = pts[(i + 1) % pts.size()];
        area += (double)a.x * b.y - (double)b.x * a.y;
    }
    if (area < 0) std::reverse(pts.begin(), pts.end());
    emit_poly_raw(edges, pts);
}

void emit_disc(std::vector<RasterEdge> &edges, Pt c, float r) {
    int n = (int)std::ceil(r * 3.0f);
    if (n < 8) n = 8;
    if (n > 40) n = 40;
    std::vector<Pt> pts;
    pts.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        float a = 6.28318530717959f * (float)i / (float)n;
        pts.push_back(Pt{ c.x + std::cos(a) * r, c.y + std::sin(a) * r });
    }
    emit_poly(edges, pts);
}

void stroke_poly(const Poly &poly, float w, int cap, int join, float miter_limit,
                 std::vector<RasterEdge> &edges) {
    float hw = w * 0.5f;
    if (hw <= 0.0f) return;
    const std::vector<Pt> &p = poly.pts;
    if (p.size() == 1) {
        if (cap == CAP_ROUND) emit_disc(edges, p[0], hw);
        else if (cap == CAP_SQUARE) {
            std::vector<Pt> q = { { p[0].x - hw, p[0].y - hw }, { p[0].x + hw, p[0].y - hw },
                                  { p[0].x + hw, p[0].y + hw }, { p[0].x - hw, p[0].y + hw } };
            emit_poly(edges, q);
        }
        return;
    }

    size_t n = p.size();
    size_t seg_count = poly.closed ? n : n - 1;
    for (size_t i = 0; i < seg_count; ++i) {
        Pt a = p[i], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        dx /= len; dy /= len;
        float nx = -dy * hw, ny = dx * hw;
        if (!poly.closed && cap == CAP_SQUARE) {
            // extend the two ends, not every segment
            if (i == 0)             { a.x -= dx * hw; a.y -= dy * hw; }
            if (i == seg_count - 1) { b.x += dx * hw; b.y += dy * hw; }
        }
        std::vector<Pt> q = { { a.x + nx, a.y + ny }, { b.x + nx, b.y + ny },
                              { b.x - nx, b.y - ny }, { a.x - nx, a.y - ny } };
        emit_poly(edges, q);
    }

    // joins at the interior vertices
    size_t first_join = poly.closed ? 0 : 1;
    size_t last_join  = poly.closed ? n : n - 1;
    for (size_t i = first_join; i < last_join; ++i) {
        Pt prev = p[(i + n - 1) % n], cur = p[i % n], next = p[(i + 1) % n];
        float d0x = cur.x - prev.x, d0y = cur.y - prev.y;
        float d1x = next.x - cur.x, d1y = next.y - cur.y;
        float l0 = std::sqrt(d0x * d0x + d0y * d0y), l1 = std::sqrt(d1x * d1x + d1y * d1y);
        if (l0 < 1e-6f || l1 < 1e-6f) continue;
        d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
        float cross = d0x * d1y - d0y * d1x;
        if (std::fabs(cross) < 1e-6f) continue;      // straight through

        if (join == JOIN_ROUND) { emit_disc(edges, cur, hw); continue; }

        // The outer side of the turn is the one the join has to fill.
        float s = cross > 0 ? -1.0f : 1.0f;
        Pt o0{ cur.x - d0y * hw * s, cur.y + d0x * hw * s };
        Pt o1{ cur.x - d1y * hw * s, cur.y + d1x * hw * s };
        if (join == JOIN_MITER) {
            float cosh2 = d0x * d1x + d0y * d1y;
            float sin_half = std::sqrt(std::max(0.0f, (1.0f - cosh2) * 0.5f));
            float ratio = sin_half > 1e-4f ? 1.0f / sin_half : 1e9f;
            if (ratio <= miter_limit) {
                float mx = o0.x + o1.x - cur.x * 2.0f, my = o0.y + o1.y - cur.y * 2.0f;
                float ml = std::sqrt(mx * mx + my * my);
                if (ml > 1e-6f) {
                    float k = hw * ratio;
                    std::vector<Pt> q = { cur, o0, { cur.x + mx / ml * k, cur.y + my / ml * k }, o1 };
                    emit_poly(edges, q);
                    continue;
                }
            }
        }
        std::vector<Pt> tri = { cur, o0, o1 };
        emit_poly(edges, tri);
    }

    if (!poly.closed && cap == CAP_ROUND) {
        emit_disc(edges, p.front(), hw);
        emit_disc(edges, p.back(), hw);
    }
}

// Scanline fill, 4x vertical supersampling, exact horizontal spans - the same
// rasteriser the font uses, minus the upside down atlas.
void fill_edges(const std::vector<RasterEdge> &edges, bool evenodd,
                std::vector<uint8_t> &cov, int w, int h) {
    cov.assign((size_t)w * h, 0);
    if (edges.empty()) return;
    const int SS = 4;
    std::vector<float> xs;
    std::vector<int> dirs, idx;
    // Float, not an integer counter. Four subsamples of 255/4 = 63.75 each,
    // truncated to 63, add up to 252 - so a shape that is completely opaque
    // never quite reaches 255, and nothing in the image is ever solid. That is
    // invisible in a screenshot and very visible in a test that asks whether
    // an icon was drawn at this size or stretched from a smaller one.
    std::vector<float> acc((size_t)w, 0.0f);
    float ymin = 1e30f, ymax = -1e30f;
    for (const RasterEdge &e : edges) {
        ymin = std::min(ymin, std::min(e.y0, e.y1));
        ymax = std::max(ymax, std::max(e.y0, e.y1));
    }
    int y_lo = std::max(0, (int)std::floor(ymin));
    int y_hi = std::min(h - 1, (int)std::ceil(ymax));

    for (int y = y_lo; y <= y_hi; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int s = 0; s < SS; ++s) {
            float sy = (float)y + ((float)s + 0.5f) / SS;
            xs.clear(); dirs.clear();
            for (const RasterEdge &e : edges) {
                float lo = std::min(e.y0, e.y1), hi = std::max(e.y0, e.y1);
                if (sy < lo || sy >= hi) continue;
                float t = (sy - e.y0) / (e.y1 - e.y0);
                xs.push_back(e.x0 + (e.x1 - e.x0) * t);
                dirs.push_back(e.dir);
            }
            if (xs.size() < 2) continue;
            idx.resize(xs.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
            std::sort(idx.begin(), idx.end(), [&](int a, int b) { return xs[a] < xs[b]; });

            int winding = 0;
            for (size_t i = 0; i + 1 < idx.size(); ++i) {
                winding += evenodd ? 1 : dirs[idx[i]];
                bool in = evenodd ? (winding & 1) != 0 : winding != 0;
                if (!in) continue;
                float xa = xs[idx[i]], xb = xs[idx[i + 1]];
                if (xb <= 0.0f || xa >= (float)w) continue;
                xa = std::max(xa, 0.0f); xb = std::min(xb, (float)w);
                if (xb <= xa) continue;
                int ia = (int)xa, ib = (int)xb;
                if (ib >= w) ib = w - 1;
                const float FULL = 255.0f / SS;
                if (ia == ib) {
                    acc[(size_t)ia] += (xb - xa) * FULL;
                } else {
                    acc[(size_t)ia] += ((float)(ia + 1) - xa) * FULL;
                    for (int x = ia + 1; x < ib; ++x) acc[(size_t)x] += FULL;
                    acc[(size_t)ib] += (xb - (float)ib) * FULL;
                }
            }
        }
        uint8_t *row = &cov[(size_t)y * w];
        for (int x = 0; x < w; ++x)
            row[x] = (uint8_t)std::min(255.0f, std::max(0.0f, acc[(size_t)x] + 0.5f));
    }
}

} // namespace

// ------------------------------------------------------------------ API

dai_svg *dai_svg_parse(const char *text, size_t len, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!text) {
        if (err && err_len) std::snprintf(err, err_len, "no svg text");
        return nullptr;
    }
    if (len == 0) len = std::strlen(text);
    dai_svg *doc = new dai_svg();
    scan_xml(doc, text, text + len);
    if (doc->shapes.empty()) {
        if (err && err_len) std::snprintf(err, err_len, "svg has no drawable geometry");
        delete doc;
        return nullptr;
    }
    return doc;
}

dai_svg *dai_svg_load(const char *path, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    std::FILE *f = std::fopen(path, "rb");
    if (!f) {
        if (err && err_len) std::snprintf(err, err_len, "cannot open %s", path ? path : "(null)");
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); if (err && err_len) std::snprintf(err, err_len, "empty file"); return nullptr; }
    std::vector<char> buf((size_t)n);
    size_t got = std::fread(buf.data(), 1, (size_t)n, f);
    std::fclose(f);
    return dai_svg_parse(buf.data(), got, err, err_len);
}

void dai_svg_free(dai_svg *s) { delete s; }

void dai_svg_viewbox(const dai_svg *s, float *x, float *y, float *w, float *h) {
    if (!s) return;
    if (x) *x = s->vx;
    if (y) *y = s->vy;
    if (w) *w = s->vw;
    if (h) *h = s->vh;
}

uint32_t dai_svg_shape_count(const dai_svg *s) { return s ? (uint32_t)s->shapes.size() : 0; }

int dai_svg_rasterize(const dai_svg *s, uint8_t *out, int w, int h, float pad) {
    if (!s || !out || w <= 0 || h <= 0) return 0;
    std::memset(out, 0, (size_t)w * h);
    if (s->vw <= 0 || s->vh <= 0) return 0;

    float aw = (float)w - pad * 2.0f, ah = (float)h - pad * 2.0f;
    if (aw <= 0 || ah <= 0) return 0;
    float scale = std::min(aw / s->vw, ah / s->vh);
    float ox = pad + (aw - s->vw * scale) * 0.5f - s->vx * scale;
    float oy = pad + (ah - s->vh * scale) * 0.5f - s->vy * scale;
    Mat xf{ scale, 0, 0, scale, ox, oy };

    std::vector<RasterEdge> edges;
    std::vector<Poly> polys;
    std::vector<uint8_t> cov;
    for (const Shape &sh : s->shapes) {
        polys.clear();
        flatten_shape(sh, xf, polys);
        if (polys.empty()) continue;

        // Fill and stroke are two passes because they obey different winding
        // rules. Combined with max() rather than added: two overlapping shapes
        // are still opaque, not 200% opaque wrapping around to nothing.
        if (sh.fill) {
            edges.clear();
            for (const Poly &p : polys) {
                if (p.pts.size() < 3) continue;
                // Filling always treats a subpath as closed - the same rule
                // the spec gives, and the reason an open 'C' shape still has
                // a filled belly.
                emit_poly_raw(edges, p.pts);
            }
            fill_edges(edges, sh.evenodd, cov, w, h);
            for (size_t i = 0; i < cov.size(); ++i) if (cov[i] > out[i]) out[i] = cov[i];
        }
        if (sh.stroke && sh.stroke_w * scale > 0.0f) {
            edges.clear();
            for (const Poly &p : polys)
                stroke_poly(p, sh.stroke_w * scale, sh.cap, sh.join, sh.miter, edges);
            fill_edges(edges, false, cov, w, h);
            for (size_t i = 0; i < cov.size(); ++i) if (cov[i] > out[i]) out[i] = cov[i];
        }
    }
    return 1;
}
