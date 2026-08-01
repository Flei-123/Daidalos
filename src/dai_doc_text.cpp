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

    // Children of a prefab instance are not written: they came from the prefab
    // file and belong to it. That is the whole point - a hundred crates are a
    // hundred lines, and fixing the crate fixes all hundred. dai_doc_nodes
    // lists parents before children, so one pass builds the skip set.
    std::unordered_map<dai_node, char> inside_prefab;
    for (dai_node id : ids) {
        const Node *n = find(d, id);
        if (!n) continue;
        bool skip = false;
        if (n->d.parent) {
            const Node *p = find(d, n->d.parent);
            // under an instance root, or under something already skipped
            if (p && (p->d.prefab[0] || inside_prefab.count(n->d.parent))) skip = true;
        }
        if (skip) inside_prefab[id] = 1;
    }

    const dai_node_desc def = dai_node_desc_default();
    for (dai_node id : ids) {
        const Node *n = find(d, id);
        if (!n) continue;
        if (inside_prefab.count(id)) continue;
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
        if (!v3eq(r.collider_center, def.collider_center)) write_v3(s, "center", r.collider_center);
        if (r.trigger != def.trigger)       put(s, "  trigger %d\n", r.trigger);
        if (!v3eq(r.render_extent, def.render_extent)) write_v3(s, "rextent", r.render_extent);
        if (!feq(r.density, def.density))       put(s, "  density %s\n", fstr(r.density).c_str());
        if (!feq(r.friction, def.friction))     put(s, "  friction %s\n", fstr(r.friction).c_str());
        if (!feq(r.restitution, def.restitution)) put(s, "  restitution %s\n", fstr(r.restitution).c_str());
        if (r.no_sleeping != def.no_sleeping)   put(s, "  nosleep %d\n", r.no_sleeping);
        if (r.no_body != def.no_body)           put(s, "  nobody %d\n", r.no_body);
        if (r.no_collider != def.no_collider)   put(s, "  nocollider %d\n", r.no_collider);
        if (r.no_rigidbody != def.no_rigidbody) put(s, "  norigidbody %d\n", r.no_rigidbody);
        if (r.script[0])                        put(s, "  script %s\n", r.script);
        if (r.mesh != def.mesh)             put(s, "  mesh %u\n", (unsigned)r.mesh);
        if (r.asset[0])                     put(s, "  asset %s\n", r.asset);
        if (r.prefab[0])                    put(s, "  prefab %s\n", r.prefab);
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
        else if (key == "center") { ok = parse_floats(after, &rec.collider_center.x, 3); }
        else if (key == "trigger") { ok = parse_i32(after, &rec.trigger); }
        else if (key == "nocollider") { ok = parse_i32(after, &rec.no_collider); }
        else if (key == "norigidbody") { ok = parse_i32(after, &rec.no_rigidbody); }
        else if (key == "script") { std::string v = rest_of_line(after);
            if (v.size() >= sizeof(rec.script)) { ok = false; }
            else std::snprintf(rec.script, sizeof(rec.script), "%s", v.c_str()); }
        else if (key == "rextent") { ok = parse_floats(after, &rec.render_extent.x, 3); }
        else if (key == "density")     { ok = parse_floats(after, &rec.density, 1); }
        else if (key == "friction")    { ok = parse_floats(after, &rec.friction, 1); }
        else if (key == "restitution") { ok = parse_floats(after, &rec.restitution, 1); }
        else if (key == "nosleep")     { ok = parse_i32(after, &rec.no_sleeping); }
        else if (key == "nobody")      { ok = parse_i32(after, &rec.no_body); }
        else if (key == "mesh")   { ok = parse_u32(after, &rec.mesh); }
        else if (key == "asset")  { std::string v = rest_of_line(after);
                                    snprintf(rec.asset, sizeof(rec.asset), "%s", v.c_str()); }
        else if (key == "prefab") { std::string v = rest_of_line(after);
                                    snprintf(rec.prefab, sizeof(rec.prefab), "%s", v.c_str()); }
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

// ---- prefabs ---------------------------------------------------------------
//
// A prefab is just a scene file, and an instance is a node that points at one.
// Expansion happens on load rather than in dai_doc_from_text, because only the
// load knows what directory the paths are relative to.

namespace {

std::string dir_of_path(const std::string &p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string join_path(const std::string &base, const std::string &rel) {
    if (rel.empty()) return rel;
    if (rel[0] == '/' || (rel.size() > 1 && rel[1] == ':')) return rel;   // absolute
    if (base.empty() || base == ".") return rel;
    return base + "/" + rel;
}

// Copies every node of `src` under `parent` in `dst`, keeping the shape of the
// tree. Ids are NOT preserved: they belong to the document they live in, and
// two instances of the same prefab must not collide.
uint32_t graft(dai_doc *dst, const dai_doc *src, dai_node parent) {
    std::vector<dai_node> ids((size_t)dai_doc_count(src));
    if (ids.empty()) return 0;
    dai_doc_nodes(src, ids.data(), (uint32_t)ids.size());

    std::unordered_map<dai_node, dai_node> map;
    uint32_t made = 0;
    for (dai_node id : ids) {                     // parents come first
        dai_node_desc rec{};
        if (dai_doc_get(src, id, &rec) != DAI_OK) continue;
        // The instance root carries the reference; the copies must not, or a
        // reload would expand them again and again.
        rec.prefab[0] = 0;
        dai_node p = parent;
        if (rec.parent) {
            auto it = map.find(rec.parent);
            if (it != map.end()) p = it->second;
        }
        rec.parent = p;
        dai_node made_id = dai_doc_add(dst, &rec);
        if (!made_id) continue;
        map[id] = made_id;
        ++made;
    }
    return made;
}

// The chain of prefab files currently being expanded. It has to be file
// scoped, not a parameter: expanding an instance calls dai_doc_load, which
// expands ITS instances, and a per call vector would start empty every time -
// a prefab containing itself would then recurse until the stack ran out.
// Which is exactly what it did.
std::vector<std::string> g_expanding;

bool expand_one(dai_doc *d, dai_node n, const std::string &base_dir,
                std::vector<std::string> &seen, char *err, size_t err_size) {
    dai_node_desc rec{};
    if (dai_doc_get(d, n, &rec) != DAI_OK || !rec.prefab[0]) return true;
    (void)seen;
    std::string full = join_path(base_dir, rec.prefab);
    for (const std::string &s : g_expanding) {
        if (s == full) {
            if (err && err_size) snprintf(err, err_size, "prefab '%s' contains itself", rec.prefab);
            return false;
        }
    }
    if (g_expanding.size() >= 8) {
        if (err && err_size) snprintf(err, err_size, "prefabs nested more than 8 deep");
        return false;
    }

    dai_doc *sub = dai_doc_create();
    if (!sub) return false;
    char lerr[192] = { 0 };
    // On the stack BEFORE the nested load, or the recursion it triggers cannot
    // see that this file is already open.
    g_expanding.push_back(full);
    dai_result loaded = dai_doc_load(sub, full.c_str(), lerr, sizeof(lerr));
    g_expanding.pop_back();
    if (loaded != DAI_OK) {
        // A missing prefab is not fatal: the instance node stays, empty and
        // obviously wrong, rather than taking the whole scene down with it.
        dai_doc_destroy(sub);
        if (err && err_size) snprintf(err, err_size, "%s", lerr);
        return true;
    }
    graft(d, sub, n);
    dai_doc_destroy(sub);
    return true;
}

uint32_t expand_all(dai_doc *d, const std::string &base_dir, char *err, size_t err_size) {
    std::vector<dai_node> ids((size_t)dai_doc_count(d));
    if (ids.empty()) return 0;
    dai_doc_nodes(d, ids.data(), (uint32_t)ids.size());
    std::vector<std::string> seen;
    uint32_t n = 0;
    for (dai_node id : ids) {
        dai_node_desc rec{};
        if (dai_doc_get(d, id, &rec) != DAI_OK || !rec.prefab[0]) continue;
        if (expand_one(d, id, base_dir, seen, err, err_size)) ++n;
    }
    return n;
}

} // namespace

dai_result dai_doc_prefab_save(const dai_doc *d, dai_node n, const char *path) {
    if (!d || !path || !dai_doc_valid(d, n)) return DAI_ERR_INVALID_ARG;

    // Copy the subtree into a document of its own, rooted at n with no parent,
    // so the file can be dropped anywhere.
    dai_doc *sub = dai_doc_create();
    if (!sub) return DAI_ERR_OUT_OF_MEMORY;

    std::vector<dai_node> ids((size_t)dai_doc_count(d));
    if (!ids.empty()) dai_doc_nodes(d, ids.data(), (uint32_t)ids.size());
    std::unordered_map<dai_node, dai_node> map;
    for (dai_node id : ids) {
        // only n and its descendants
        bool mine = (id == n);
        if (!mine) {
            dai_node_desc probe{};
            if (dai_doc_get(d, id, &probe) != DAI_OK) continue;
            if (probe.parent && (probe.parent == n || map.count(probe.parent))) mine = true;
        }
        if (!mine) continue;

        dai_node_desc rec{};
        if (dai_doc_get(d, id, &rec) != DAI_OK) continue;
        if (id == n) {
            rec.parent = 0;
            rec.prefab[0] = 0;     // the original is not an instance of itself
        } else {
            auto it = map.find(rec.parent);
            rec.parent = it == map.end() ? 0 : it->second;
        }
        dai_node made = dai_doc_add(sub, &rec);
        if (made) map[id] = made;
    }
    dai_result r = dai_doc_save(sub, path);
    dai_doc_destroy(sub);
    return r;
}

dai_node dai_doc_prefab_instantiate(dai_doc *d, const char *path, dai_node parent,
                                    const char *base_dir, char *err, size_t err_size) {
    if (!d || !path || !path[0]) return 0;
    if (err && err_size) err[0] = 0;

    std::string full = join_path(base_dir ? base_dir : ".", path);
    dai_doc *sub = dai_doc_create();
    if (!sub) return 0;
    if (dai_doc_load(sub, full.c_str(), err, err_size) != DAI_OK) {
        dai_doc_destroy(sub);
        return 0;
    }
    // The instance root is a transform node that points at the file. It gets
    // the prefab root's own transform so the instance lands where the original
    // was authored.
    dai_node_desc root = dai_node_desc_default();
    std::vector<dai_node> sids((size_t)dai_doc_count(sub));
    if (!sids.empty()) {
        dai_doc_nodes(sub, sids.data(), (uint32_t)sids.size());
        dai_doc_get(sub, sids[0], &root);
    }
    root.parent = parent;
    root.no_body = 1;                 // the pieces carry the physics
    root.mesh = 0xFFFFFFFFu;
    root.asset[0] = 0;
    snprintf(root.prefab, sizeof(root.prefab), "%s", path);

    dai_doc_begin(d, "Instantiate prefab");
    dai_node made = dai_doc_add(d, &root);
    if (made) graft(d, sub, made);
    dai_doc_commit(d);
    dai_doc_destroy(sub);
    return made;
}

uint32_t dai_doc_prefab_reload(dai_doc *d, const char *base_dir) {
    if (!d) return 0;
    std::vector<dai_node> ids((size_t)dai_doc_count(d));
    if (ids.empty()) return 0;
    dai_doc_nodes(d, ids.data(), (uint32_t)ids.size());

    dai_doc_begin(d, "Reload prefabs");
    uint32_t n = 0;
    std::vector<std::string> seen;
    for (dai_node id : ids) {
        dai_node_desc rec{};
        if (dai_doc_get(d, id, &rec) != DAI_OK || !rec.prefab[0]) continue;
        // Drop what is there and take it from disk again.
        dai_node kids[256];
        uint32_t kn = dai_doc_children(d, id, kids, 256);
        for (uint32_t i = 0; i < kn; ++i) dai_doc_remove(d, kids[i]);
        char lerr[192] = { 0 };
        expand_one(d, id, base_dir ? base_dir : ".", seen, lerr, sizeof(lerr));
        ++n;
    }
    dai_doc_commit(d);
    return n;
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
    dai_result r = dai_doc_from_text(d, data.c_str(), data.size(), err, err_size);
    if (r != DAI_OK) return r;
    // Prefab references are relative to the scene that holds them, so this can
    // only happen here - dai_doc_from_text has no idea where the text came from.
    expand_all(d, dir_of_path(path), err, err_size);
    return DAI_OK;
}

} // extern "C"
