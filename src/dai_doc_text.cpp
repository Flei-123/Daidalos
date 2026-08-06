// Text serialisation of the scene document.
//
// Line based, one key per line, only non default fields written. Chosen over
// JSON because a scene file lives in version control: this diffs per property
// instead of per brace, and a three way merge of two people editing different
// objects actually resolves. Round trip is exact - floats print at the shortest
// precision that still reads back bit identical (see fstr below).

#ifdef _WIN32
#include <windows.h>
#endif
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
        // The tag was the one field the format forgot, and it is not
        // decoration: dai_editor_ui finds the game camera BY TAG, so a
        // scene that lost it came back with no camera at all - and the
        // shipped game had nothing to render from.
        if (r.tag[0])                       put(s, "  tag %s\n", r.tag);
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
        if (r.materials[0])                 put(s, "  materials %s\n", r.materials);
        if (r.prefab[0])                    put(s, "  prefab %s\n", r.prefab);
        if (!v3eq(r.color, def.color))          write_v3(s, "color", r.color);
        if (!feq(r.roughness, def.roughness))   put(s, "  roughness %s\n", fstr(r.roughness).c_str());
        if (!feq(r.emissive, def.emissive))     put(s, "  emissive %s\n", fstr(r.emissive).c_str());
        // Optional components. Only written when present, so a scene that has
        // none reads exactly as it did before these existed.
        if (r.camera != def.camera)   put(s, "  camera %d\n", r.camera);
        if (!feq(r.camera_fov, def.camera_fov))   put(s, "  camfov %s\n", fstr(r.camera_fov).c_str());
        if (!feq(r.camera_size, def.camera_size)) put(s, "  camsize %s\n", fstr(r.camera_size).c_str());
        if (r.light != def.light)     put(s, "  light %d\n", r.light);
        if (!feq(r.light_color.x, def.light_color.x) || !feq(r.light_color.y, def.light_color.y) ||
            !feq(r.light_color.z, def.light_color.z))
            put(s, "  lcolor %s %s %s\n", fstr(r.light_color.x).c_str(),
                fstr(r.light_color.y).c_str(), fstr(r.light_color.z).c_str());
        if (!feq(r.light_range, def.light_range))         put(s, "  lrange %s\n", fstr(r.light_range).c_str());
        if (!feq(r.light_intensity, def.light_intensity)) put(s, "  lpower %s\n", fstr(r.light_intensity).c_str());
        if (!feq(r.light_cone, def.light_cone))           put(s, "  lcone %s\n", fstr(r.light_cone).c_str());
        if (r.freeze != def.freeze)   put(s, "  freeze %u\n", (unsigned)r.freeze);
        if (r.sprite != def.sprite)   put(s, "  sprite %d\n", r.sprite);
        if (!feq(r.sprite_size.x, def.sprite_size.x) || !feq(r.sprite_size.y, def.sprite_size.y) ||
            !feq(r.sprite_size.z, def.sprite_size.z))
            put(s, "  spsize %s %s %s\n", fstr(r.sprite_size.x).c_str(),
                fstr(r.sprite_size.y).c_str(), fstr(r.sprite_size.z).c_str());
        // Text. The words go last on their line, so they may contain spaces -
        // and they must, because "Press any key" is one string, not three.
        if (r.text_on != def.text_on)     put(s, "  text %d\n", r.text_on);
        if (r.text[0])                    put(s, "  textstr %s\n", r.text);
        if (!feq(r.text_size, def.text_size)) put(s, "  textsize %s\n", fstr(r.text_size).c_str());
        if (!feq(r.text_color.x, def.text_color.x) || !feq(r.text_color.y, def.text_color.y) ||
            !feq(r.text_color.z, def.text_color.z))
            put(s, "  textcol %s %s %s\n", fstr(r.text_color.x).c_str(),
                fstr(r.text_color.y).c_str(), fstr(r.text_color.z).c_str());
        if (r.text_anchor != def.text_anchor) put(s, "  textanchor %d\n", r.text_anchor);
        if (r.text_font[0])                   put(s, "  textfont %s\n", r.text_font);
        if (!feq(r.text_w, def.text_w) || !feq(r.text_h, def.text_h))
            put(s, "  textbox %s %s\n", fstr(r.text_w).c_str(), fstr(r.text_h).c_str());
        if (r.text_autosize != def.text_autosize) put(s, "  textfit %d\n", r.text_autosize);
        if (r.image_on != def.image_on)       put(s, "  image %d\n", r.image_on);
        if (r.image[0])                       put(s, "  imagefile %s\n", r.image);
        if (!feq(r.image_w, def.image_w) || !feq(r.image_h, def.image_h))
            put(s, "  imagesize %s %s\n", fstr(r.image_w).c_str(), fstr(r.image_h).c_str());
        if (!feq(r.image_color.x, def.image_color.x) || !feq(r.image_color.y, def.image_color.y) ||
            !feq(r.image_color.z, def.image_color.z))
            put(s, "  imagecol %s %s %s\n", fstr(r.image_color.x).c_str(),
                fstr(r.image_color.y).c_str(), fstr(r.image_color.z).c_str());
        if (r.image_anchor != def.image_anchor) put(s, "  imageanchor %d\n", r.image_anchor);
        if (!feq(r.image_x, def.image_x) || !feq(r.image_y, def.image_y))
            put(s, "  imagepos %s %s\n", fstr(r.image_x).c_str(), fstr(r.image_y).c_str());
        if (!feq(r.text_x, def.text_x) || !feq(r.text_y, def.text_y))
            put(s, "  textpos %s %s\n", fstr(r.text_x).c_str(), fstr(r.text_y).c_str());
        if (r.button_on != def.button_on)  put(s, "  button %d\n", r.button_on);
        if (r.button_action[0])            put(s, "  buttonact %s\n", r.button_action);
        if (!feq(r.button_hover.x, def.button_hover.x) ||
            !feq(r.button_hover.y, def.button_hover.y) ||
            !feq(r.button_hover.z, def.button_hover.z))
            put(s, "  buttonhover %s %s %s\n", fstr(r.button_hover.x).c_str(),
                fstr(r.button_hover.y).c_str(), fstr(r.button_hover.z).c_str());
        if (!feq(r.button_press.x, def.button_press.x) ||
            !feq(r.button_press.y, def.button_press.y) ||
            !feq(r.button_press.z, def.button_press.z))
            put(s, "  buttonpress %s %s %s\n", fstr(r.button_press.x).c_str(),
                fstr(r.button_press.y).c_str(), fstr(r.button_press.z).c_str());
        if (r.audio_event[0])            put(s, "  audio %s\n", r.audio_event);
        if (r.audio_bus != def.audio_bus) put(s, "  abus %d\n", r.audio_bus);
        if (!feq(r.audio_volume, def.audio_volume)) put(s, "  avol %s\n", fstr(r.audio_volume).c_str());
        if (r.audio_loop != def.audio_loop)         put(s, "  aloop %d\n", r.audio_loop);
        if (r.audio_autoplay != def.audio_autoplay) put(s, "  aplay %d\n", r.audio_autoplay);
        if (r.render_flags != def.render_flags) put(s, "  rflags %u\n", (unsigned)r.render_flags);
        if (r.hidden != def.hidden)             put(s, "  hidden %d\n", r.hidden);
        if (r.disabled != def.disabled)         put(s, "  disabled %d\n", r.disabled);
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
        else if (key == "tag")    { std::string v = rest_of_line(after);
                                    snprintf(rec.tag, sizeof(rec.tag), "%s", v.c_str()); }
        else if (key == "parent") { ok = parse_u32(after, &rec.parent); }
        else if (key == "pos")    { ok = parse_floats(after, &rec.position.x, 3); }
        else if (key == "rot")    { ok = parse_floats(after, &rec.rotation.x, 4); }
        else if (key == "scale")  { ok = parse_floats(after, &rec.scale.x, 3); }
        // Range checked, unlike every other integer here, because an out of
        // range shape does not fail - it falls through to the backend's
        // default and quietly becomes a box. A scene written by a newer build
        // (a shape this one does not have) would then load, look wrong and say
        // nothing, which is the worst of the three possible outcomes.
        else if (key == "shape")  { ok = parse_i32(after, &rec.shape) &&
                                         rec.shape >= DAI_SHAPE_BOX &&
                                         rec.shape <= DAI_SHAPE_CYLINDER; }
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
        else if (key == "materials") { std::string v = rest_of_line(after);
                                    snprintf(rec.materials, sizeof(rec.materials), "%s", v.c_str()); }
        else if (key == "color")  { ok = parse_floats(after, &rec.color.x, 3); }
        else if (key == "roughness") { ok = parse_floats(after, &rec.roughness, 1); }
        else if (key == "emissive")  { ok = parse_floats(after, &rec.emissive, 1); }
        else if (key == "camera")  { ok = parse_i32(after, &rec.camera); }
        else if (key == "camfov")  { ok = parse_floats(after, &rec.camera_fov, 1); }
        else if (key == "camsize") { ok = parse_floats(after, &rec.camera_size, 1); }
        else if (key == "light")   { ok = parse_i32(after, &rec.light); }
        else if (key == "lcolor")  { ok = parse_floats(after, &rec.light_color.x, 3); }
        else if (key == "lrange")  { ok = parse_floats(after, &rec.light_range, 1); }
        else if (key == "lpower")  { ok = parse_floats(after, &rec.light_intensity, 1); }
        else if (key == "lcone")   { ok = parse_floats(after, &rec.light_cone, 1); }
        else if (key == "text")    { ok = parse_i32(after, &rec.text_on); }
        else if (key == "textstr") {
            // Everything after the keyword, spaces included, trailing
            // whitespace off. A label is one value, not a word list.
            const char *b = after;
            while (*b == ' ' || *b == '\t') ++b;      // token() leaves the separator
            std::string v = b;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
            if (v.size() >= sizeof(rec.text)) ok = false;
            else std::snprintf(rec.text, sizeof(rec.text), "%s", v.c_str()); }
        else if (key == "textsize")   { ok = parse_floats(after, &rec.text_size, 1); }
        else if (key == "textcol")    { ok = parse_floats(after, &rec.text_color.x, 3); }
        else if (key == "textanchor") { ok = parse_i32(after, &rec.text_anchor); }
        else if (key == "textbox")    { ok = parse_floats(after, &rec.text_w, 2); }
        else if (key == "textfit")    { ok = parse_i32(after, &rec.text_autosize); }
        else if (key == "image")      { ok = parse_i32(after, &rec.image_on); }
        else if (key == "imagesize")  { ok = parse_floats(after, &rec.image_w, 2); }
        else if (key == "imagecol")   { ok = parse_floats(after, &rec.image_color.x, 3); }
        else if (key == "imageanchor"){ ok = parse_i32(after, &rec.image_anchor); }
        else if (key == "imagepos")   { ok = parse_floats(after, &rec.image_x, 2); }
        else if (key == "button")     { ok = parse_i32(after, &rec.button_on); }
        else if (key == "buttonhover"){ ok = parse_floats(after, &rec.button_hover.x, 3); }
        else if (key == "buttonpress"){ ok = parse_floats(after, &rec.button_press.x, 3); }
        else if (key == "buttonact") {
            std::string v = after;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
            if (v.size() >= sizeof(rec.button_action)) ok = false;
            else std::snprintf(rec.button_action, sizeof(rec.button_action), "%s", v.c_str()); }
        else if (key == "imagefile") {
            std::string v = after;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
            if (v.size() >= sizeof(rec.image)) ok = false;
            else std::snprintf(rec.image, sizeof(rec.image), "%s", v.c_str()); }
        else if (key == "textfont") {
            std::string v = after;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
            if (v.size() >= sizeof(rec.text_font)) ok = false;
            else std::snprintf(rec.text_font, sizeof(rec.text_font), "%s", v.c_str()); }
        else if (key == "textpos")    { ok = parse_floats(after, &rec.text_x, 2); }
        else if (key == "freeze")  { int fv = 0; ok = parse_i32(after, &fv); rec.freeze = (uint32_t)fv; }
        else if (key == "sprite")  { ok = parse_i32(after, &rec.sprite); }
        else if (key == "spsize")  { ok = parse_floats(after, &rec.sprite_size.x, 3); }
        else if (key == "audio")   { std::string v = rest_of_line(after);
            if (v.size() >= sizeof(rec.audio_event)) ok = false;
            else std::snprintf(rec.audio_event, sizeof(rec.audio_event), "%s", v.c_str()); }
        else if (key == "abus")    { ok = parse_i32(after, &rec.audio_bus); }
        else if (key == "avol")    { ok = parse_floats(after, &rec.audio_volume, 1); }
        else if (key == "aloop")   { ok = parse_i32(after, &rec.audio_loop); }
        else if (key == "aplay")   { ok = parse_i32(after, &rec.audio_autoplay); }
        else if (key == "rflags") { ok = parse_u32(after, &rec.render_flags); }
        else if (key == "hidden") { ok = parse_i32(after, &rec.hidden); }
        else if (key == "disabled") { ok = parse_i32(after, &rec.disabled); }
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
#ifdef _WIN32
    // The Windows CRT rename() does NOT replace an existing target - the first
    // save works, every Ctrl+S after that fails and the scene silently stays
    // as it was on disk. MoveFileEx with REPLACE_EXISTING behaves like POSIX.
    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp.c_str());
        return DAI_ERR_FILE;
    }
#else
    if (rename(tmp.c_str(), path) != 0) { remove(tmp.c_str()); return DAI_ERR_FILE; }
#endif
    return DAI_OK;
}

// ---- prefabs ---------------------------------------------------------------
//
// Copies every node of `sub` EXCEPT `skip_root` under `into`, keeping the
// shape: a child of the skipped root becomes a child of `into`.
static void graft_children(dai_doc *d, const dai_doc *sub, dai_node skip_root, dai_node into) {
    if (!d || !sub || !into) return;
    std::vector<dai_node> ids((size_t)dai_doc_count(sub));
    if (ids.empty()) return;
    dai_doc_nodes(sub, ids.data(), (uint32_t)ids.size());
    std::unordered_map<dai_node, dai_node> map;
    map[skip_root] = into;              // the root maps onto the instance itself
    for (dai_node id : ids) {
        if (id == skip_root) continue;
        dai_node_desc rec{};
        if (dai_doc_get(sub, id, &rec) != DAI_OK) continue;
        auto it = map.find(rec.parent);
        rec.parent = it == map.end() ? into : it->second;
        rec.prefab[0] = 0;              // only the instance root points at the file
        dai_node made = dai_doc_add(d, &rec);
        if (made) map[id] = made;
    }
}

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
    // The node that carries the reference IS the instance root - exactly the
    // rule dai_doc_prefab_instantiate follows. Grafting the WHOLE file under
    // it put the empty wrapper back on every load: place a prefab, save,
    // reopen, and the crate has grown a parent it did not have a second ago,
    // and every click selects that parent instead of the crate.
    //
    // The instance's own record is complete in the scene file - the save
    // skips the CHILDREN of an instance, not the instance - so only the
    // children have to come back.
    std::vector<dai_node> sids((size_t)dai_doc_count(sub));
    if (!sids.empty()) dai_doc_nodes(sub, sids.data(), (uint32_t)sids.size());
    graft_children(d, sub, sids.empty() ? 0 : sids[0], n);
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
            // At its OWN origin, never where it happened to be standing when
            // it was made. A prefab that carries the world position of the
            // crate you dragged it from drops every future instance in that
            // one spot, and the children - which are stored relative to this
            // root - are the only things that were ever meant to be offsets.
            rec.position = dai_vec3{ 0, 0, 0 };
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
    std::vector<dai_node> sids((size_t)dai_doc_count(sub));
    if (!sids.empty()) dai_doc_nodes(sub, sids.data(), (uint32_t)sids.size());

    // A prefab of ONE object instantiates as ONE object.
    //
    // It used to always build a wrapper: an empty transform pointing at the
    // file, with the prefab's contents grafted underneath. For a crate made of
    // twelve pieces that is right - they need something to hang off. For a
    // prefab of a single cube it is a box inside a box, and the thing you
    // select, move and look at in the inspector is the empty one, which has no
    // mesh, no collider and nothing to edit. Unity gives you the cube.
    //
    // So: when the file holds exactly one node, THAT node is the instance and
    // carries the prefab link itself.
    if (sids.size() == 1) {
        dai_node_desc only = dai_node_desc_default();
        dai_doc_get(sub, sids[0], &only);
        only.parent = parent;
        snprintf(only.prefab, sizeof(only.prefab), "%s", path);
        dai_doc_begin(d, "Instantiate prefab");
        dai_node made1 = dai_doc_add(d, &only);
        dai_doc_commit(d);
        dai_doc_destroy(sub);
        return made1;
    }

    // The prefab's OWN root is the instance root - never an extra node above
    // it. The wrapper was convenient (somewhere to hang the children and the
    // file reference) and wrong: instantiate a prefab twice, or make a prefab
    // of an instance, and you get a parent inside a parent inside a parent,
    // each one empty, each one the thing your click actually selects.
    //
    // The root keeps its mesh, its collider and its transform; it just also
    // carries the prefab path. Its children come from the file underneath it.
    dai_node_desc root = dai_node_desc_default();
    if (!sids.empty()) dai_doc_get(sub, sids[0], &root);
    root.parent = parent;
    snprintf(root.prefab, sizeof(root.prefab), "%s", path);

    dai_doc_begin(d, "Instantiate prefab");
    dai_node made = dai_doc_add(d, &root);
    // Everything except the root, re-parented under the new instance.
    if (made) graft_children(d, sub, sids.empty() ? 0 : sids[0], made);
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
