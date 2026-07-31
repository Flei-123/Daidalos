// Text serialisation of the scene document.
//
// Line based, one key per line, only non default fields written. Chosen over
// JSON because a scene file lives in version control: this diffs per property
// instead of per brace, and a three way merge of two people editing different
// objects actually resolves. Round trip is exact - floats print at the shortest
// precision that still reads back bit identical (see fstr below).

#include "dai_doc.h"
#include "dai_doc_internal.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace daidoc;

namespace {

const int  FORMAT_VERSION = 1;
const char MAGIC[] = "daidalos-scene";

void put(std::string &s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

// Shortest representation that still reads back bit identical. %.9g is always
// exact but writes 1.20000005 for a scale of 1.2, which makes a hand written
// scene file look broken and a diff unreadable.
std::string fstr(float v) {
    char buf[40];
    for (int prec = 6; prec < 9; ++prec) {
        snprintf(buf, sizeof(buf), "%.*g", prec, (double)v);
        if ((float)strtod(buf, nullptr) == v) return buf;
    }
    snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return buf;
}

bool feq(float a, float b) { return a == b; }
bool v3eq(dai_vec3 a, dai_vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool qeq(dai_quat a, dai_quat b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

void write_v3(std::string &s, const char *key, dai_vec3 v) {
    put(s, "  %s %s %s %s\n", key, fstr(v.x).c_str(), fstr(v.y).c_str(), fstr(v.z).c_str());
}

// ---- parsing helpers ----------------------------------------------------

struct Line {
    const char *p = nullptr;
    int         no = 0;
};

void fail(char *err, size_t err_size, int line, const char *msg, const char *what = nullptr) {
    if (!err || !err_size) return;
    if (what) snprintf(err, err_size, "line %d: %s '%s'", line, msg, what);
    else      snprintf(err, err_size, "line %d: %s", line, msg);
}

const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// Reads a token into `out`, returns the position after it.
const char *token(const char *p, std::string &out) {
    p = skip_ws(p);
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    out.assign(start, (size_t)(p - start));
    return p;
}

bool parse_floats(const char *p, float *out, int count) {
    for (int i = 0; i < count; ++i) {
        p = skip_ws(p);
        if (!*p || *p == '\r' || *p == '\n') return false;
        char *endp = nullptr;
        double v = strtod(p, &endp);
        if (endp == p) return false;
        if (!std::isfinite(v)) return false;     // NaN in a scene file is corruption
        out[i] = (float)v;
        p = endp;
    }
    return true;
}

bool parse_u32(const char *p, uint32_t *out) {
    p = skip_ws(p);
    char *endp = nullptr;
    unsigned long v = strtoul(p, &endp, 10);
    if (endp == p) return false;
    *out = (uint32_t)v;
    return true;
}

bool parse_i32(const char *p, int *out) {
    p = skip_ws(p);
    char *endp = nullptr;
    long v = strtol(p, &endp, 10);
    if (endp == p) return false;
    *out = (int)v;
    return true;
}

// The rest of the line, trailing whitespace trimmed. Names may contain spaces.
std::string rest_of_line(const char *p) {
    p = skip_ws(p);
    std::string s(p);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    return s;
}

} // namespace

extern "C" {

size_t dai_doc_to_text(const dai_doc *d, char *buf, size_t buf_size) {
    if (!d) return 0;
    std::string s;
    put(s, "%s %d\n", MAGIC, FORMAT_VERSION);
    put(s, "next-id %u\n", (unsigned)d->next_id);

    std::vector<dai_node> ids((size_t)dai_doc_count(d));
    if (!ids.empty()) dai_doc_nodes(d, ids.data(), (uint32_t)ids.size());

    const dai_node_desc def = dai_node_desc_default();
    for (dai_node id : ids) {
        const Node *n = find(d, id);
        if (!n) continue;
        const dai_node_desc &r = n->d;
        put(s, "\nnode %u\n", (unsigned)id);
        if (r.name[0])                      put(s, "  name %s\n", r.name);
        if (r.parent)                       put(s, "  parent %u\n", (unsigned)r.parent);
        if (!v3eq(r.position, def.position))    write_v3(s, "pos", r.position);
        if (!qeq(r.rotation, def.rotation))
            put(s, "  rot %s %s %s %s\n", fstr(r.rotation.x).c_str(), fstr(r.rotation.y).c_str(),
                fstr(r.rotation.z).c_str(), fstr(r.rotation.w).c_str());
        if (!v3eq(r.scale, def.scale))          write_v3(s, "scale", r.scale);
        if (r.shape != def.shape)           put(s, "  shape %d\n", r.shape);
        if (r.motion != def.motion)         put(s, "  motion %d\n", r.motion);
        if (!v3eq(r.half_extent, def.half_extent)) write_v3(s, "extent", r.half_extent);
        if (!feq(r.density, def.density))       put(s, "  density %s\n", fstr(r.density).c_str());
        if (!feq(r.friction, def.friction))     put(s, "  friction %s\n", fstr(r.friction).c_str());
        if (!feq(r.restitution, def.restitution)) put(s, "  restitution %s\n", fstr(r.restitution).c_str());
        if (r.no_sleeping != def.no_sleeping)   put(s, "  nosleep %d\n", r.no_sleeping);
        if (r.no_body != def.no_body)           put(s, "  nobody %d\n", r.no_body);
        if (r.mesh != def.mesh)             put(s, "  mesh %u\n", (unsigned)r.mesh);
        if (!v3eq(r.color, def.color))          write_v3(s, "color", r.color);
        if (!feq(r.roughness, def.roughness))   put(s, "  roughness %s\n", fstr(r.roughness).c_str());
        if (!feq(r.emissive, def.emissive))     put(s, "  emissive %s\n", fstr(r.emissive).c_str());
        if (r.render_flags != def.render_flags) put(s, "  rflags %u\n", (unsigned)r.render_flags);
        if (r.hidden != def.hidden)             put(s, "  hidden %d\n", r.hidden);
        if (r.user_data != def.user_data)       put(s, "  user %u\n", (unsigned)r.user_data);
        put(s, "end\n");
    }

    if (buf && buf_size) {
        size_t n = s.size() < buf_size - 1 ? s.size() : buf_size - 1;
        std::memcpy(buf, s.c_str(), n);
        buf[n] = 0;
    }
    return s.size();
}

dai_result dai_doc_from_text(dai_doc *d, const char *text, size_t len,
                             char *err, size_t err_size) {
    if (!d || !text) return DAI_ERR_INVALID_ARG;
    if (err && err_size) err[0] = 0;

    // Parse into a scratch map first. A half applied load is worse than a
    // rejected one: the user would lose the scene they still had open.
    std::unordered_map<dai_node, Node> parsed;
    dai_node next_id = 1;
    bool seen_header = false;
    dai_node current = DAI_INVALID_NODE;
    dai_node_desc rec{};

    std::string src(text, len);
    size_t pos = 0;
    int line_no = 0;
    std::string key;

    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        std::string line = src.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? src.size() + 1 : nl + 1;
        ++line_no;

        const char *p = skip_ws(line.c_str());
        if (!*p || *p == '\r' || *p == '#') continue;
        const char *after = token(p, key);

        if (!seen_header) {
            if (key != MAGIC) { fail(err, err_size, line_no, "not a daidalos scene"); return DAI_ERR_FILE; }
            int ver = 0;
            if (!parse_i32(after, &ver)) { fail(err, err_size, line_no, "missing format version"); return DAI_ERR_FILE; }
            if (ver > FORMAT_VERSION) {
                fail(err, err_size, line_no, "scene is newer than this build");
                return DAI_ERR_FILE;
            }
            seen_header = true;
            continue;
        }

        if (key == "next-id") {
            if (!parse_u32(after, &next_id)) { fail(err, err_size, line_no, "bad next-id"); return DAI_ERR_FILE; }
            continue;
        }

        if (key == "node") {
            if (current) { fail(err, err_size, line_no, "node inside a node"); return DAI_ERR_FILE; }
            uint32_t id = 0;
            if (!parse_u32(after, &id) || id == 0) { fail(err, err_size, line_no, "bad node id"); return DAI_ERR_FILE; }
            if (parsed.find(id) != parsed.end()) { fail(err, err_size, line_no, "duplicate node id"); return DAI_ERR_FILE; }
            current = id;
            rec = dai_node_desc_default();
            continue;
        }

        if (key == "end") {
            if (!current) { fail(err, err_size, line_no, "end without node"); return DAI_ERR_FILE; }
            Node n;
            n.d = rec;
            n.alive = true;
            parsed[current] = n;
            current = DAI_INVALID_NODE;
            continue;
        }

        if (!current) { fail(err, err_size, line_no, "key outside a node:", key.c_str()); return DAI_ERR_FILE; }

        bool ok = true;
        if      (key == "name")   { std::string v = rest_of_line(after);
                                    snprintf(rec.name, sizeof(rec.name), "%s", v.c_str()); }
        else if (key == "parent") { ok = parse_u32(after, &rec.parent); }
        else if (key == "pos")    { ok = parse_floats(after, &rec.position.x, 3); }
        else if (key == "rot")    { ok = parse_floats(after, &rec.rotation.x, 4); }
        else if (key == "scale")  { ok = parse_floats(after, &rec.scale.x, 3); }
        else if (key == "shape")  { ok = parse_i32(after, &rec.shape); }
        else if (key == "motion") { ok = parse_i32(after, &rec.motion); }
        else if (key == "extent") { ok = parse_floats(after, &rec.half_extent.x, 3); }
        else if (key == "density")     { ok = parse_floats(after, &rec.density, 1); }
        else if (key == "friction")    { ok = parse_floats(after, &rec.friction, 1); }
        else if (key == "restitution") { ok = parse_floats(after, &rec.restitution, 1); }
        else if (key == "nosleep")     { ok = parse_i32(after, &rec.no_sleeping); }
        else if (key == "nobody")      { ok = parse_i32(after, &rec.no_body); }
        else if (key == "mesh")   { ok = parse_u32(after, &rec.mesh); }
        else if (key == "color")  { ok = parse_floats(after, &rec.color.x, 3); }
        else if (key == "roughness") { ok = parse_floats(after, &rec.roughness, 1); }
        else if (key == "emissive")  { ok = parse_floats(after, &rec.emissive, 1); }
        else if (key == "rflags") { ok = parse_u32(after, &rec.render_flags); }
        else if (key == "hidden") { ok = parse_i32(after, &rec.hidden); }
        else if (key == "user")   { ok = parse_u32(after, &rec.user_data); }
        else {
            // Strict on purpose: silently swallowing an unknown key turns a
            // typo into data loss the next time the file is saved.
            fail(err, err_size, line_no, "unknown key", key.c_str());
            return DAI_ERR_FILE;
        }
        if (!ok) { fail(err, err_size, line_no, "bad value for", key.c_str()); return DAI_ERR_FILE; }
    }

    if (!seen_header) { fail(err, err_size, 1, "empty or truncated file"); return DAI_ERR_FILE; }
    if (current)      { fail(err, err_size, line_no, "unterminated node (missing 'end')"); return DAI_ERR_FILE; }

    // Validate the hierarchy before committing: a dangling parent would make
    // world transforms silently wrong instead of loudly rejected.
    for (const auto &kv : parsed) {
        dai_node p = kv.second.d.parent;
        if (p && parsed.find(p) == parsed.end()) {
            fail(err, err_size, 0, "node refers to a missing parent");
            return DAI_ERR_FILE;
        }
        size_t guard = parsed.size() + 1;
        while (p && guard--) {
            if (p == kv.first) { fail(err, err_size, 0, "parent cycle in scene"); return DAI_ERR_FILE; }
            auto it = parsed.find(p);
            p = (it == parsed.end()) ? 0 : it->second.d.parent;
        }
        if (kv.first >= next_id) next_id = kv.first + 1;   // never hand out a used id
    }

    dai_doc_clear(d);
    d->nodes = std::move(parsed);
    d->next_id = next_id;
    for (auto &kv : d->nodes) kv.second.rev = ++d->rev_counter;
    d->revision++;
    return DAI_OK;
}

dai_result dai_doc_save(const dai_doc *d, const char *path) {
    if (!d || !path) return DAI_ERR_INVALID_ARG;
    size_t need = dai_doc_to_text(d, nullptr, 0);
    std::string buf(need + 1, '\0');
    dai_doc_to_text(d, &buf[0], buf.size());

    // Write to a temp file and rename: a crash mid save must not leave a
    // truncated scene where the user's work used to be.
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return DAI_ERR_FILE;
    size_t written = fwrite(buf.c_str(), 1, need, f);
    int flushed = fflush(f);
    fclose(f);
    if (written != need || flushed != 0) { remove(tmp.c_str()); return DAI_ERR_FILE; }
    if (rename(tmp.c_str(), path) != 0) { remove(tmp.c_str()); return DAI_ERR_FILE; }
    return DAI_OK;
}

dai_result dai_doc_load(dai_doc *d, const char *path, char *err, size_t err_size) {
    if (!d || !path) return DAI_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && err_size) snprintf(err, err_size, "cannot open '%s'", path);
        return DAI_ERR_FILE;
    }
    std::string data;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data.append(chunk, n);
    fclose(f);
    return dai_doc_from_text(d, data.c_str(), data.size(), err, err_size);
}

} // extern "C"
