# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'examples/editor_demo.cpp'
s = rw(P)

s = sub1(s,
"""static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot, nullptr };""",
"""static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot, nullptr };

// ---- native (C++) behaviours -------------------------------------------
//
// The same play/stop lifetime as the .js ones: a .cpp attached to a node is
// compiled to a shared library the first time Play is pressed after it
// changed, loaded, and called every frame. The API it sees is the C struct in
// dai_native.h - see that file for why it is not a base class.
static dai_native *g_native = nullptr;
static float       g_native_time = 0.0f;
static dai_window *g_win_for_scripts = nullptr;

static void nv_log(const dai_native_api *, const char *text) {
    if (!text) return;
    std::printf("cpp: %s\\n", text);
    if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 0, text);
}
static dai_nvec3 nv_get_pos(const dai_native_api *, dai_entity e) {
    dai_vec3 p{};
    if (g_script_ed) dai_editor_live_transform(g_script_ed, (dai_node)e, &p, nullptr, nullptr);
    return dai_nvec3{ p.x, p.y, p.z };
}
static void nv_set_pos(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    if (!g_script_ed) return;
    dai_vec3 p{ v.x, v.y, v.z };
    dai_editor_live_set_transform(g_script_ed, (dai_node)e, &p, nullptr);
}
static dai_nvec3 nv_get_vel(const dai_native_api *, dai_entity e) {
    dai_vec3 l{}, a{};
    if (g_script_ed) dai_editor_live_velocity(g_script_ed, (dai_node)e, &l, &a);
    return dai_nvec3{ l.x, l.y, l.z };
}
static void nv_set_vel(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    if (g_script_ed) dai_editor_live_set_velocity(g_script_ed, (dai_node)e, dai_vec3{ v.x, v.y, v.z });
}
static void nv_impulse(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    if (g_script_ed) dai_editor_live_impulse(g_script_ed, (dai_node)e, dai_vec3{ v.x, v.y, v.z });
}
static dai_nvec3 nv_get_scale(const dai_native_api *, dai_entity e) {
    dai_node_desc r{};
    if (g_scene_doc && dai_doc_get(g_scene_doc, (dai_node)e, &r) == DAI_OK)
        return dai_nvec3{ r.scale.x, r.scale.y, r.scale.z };
    return dai_nvec3{ 1, 1, 1 };
}
static void nv_set_scale(const dai_native_api *, dai_entity e, dai_nvec3 v) {
    dai_node_desc r{};
    if (!g_scene_doc || dai_doc_get(g_scene_doc, (dai_node)e, &r) != DAI_OK) return;
    r.scale = dai_vec3{ v.x, v.y, v.z };
    dai_doc_set(g_scene_doc, (dai_node)e, &r);
}
static void nv_get_rot(const dai_native_api *, dai_entity e, float *xyzw) {
    dai_quat q{ 0, 0, 0, 1 };
    if (g_script_ed) dai_editor_live_transform(g_script_ed, (dai_node)e, nullptr, &q, nullptr);
    if (xyzw) { xyzw[0] = q.x; xyzw[1] = q.y; xyzw[2] = q.z; xyzw[3] = q.w; }
}
static void nv_set_rot(const dai_native_api *, dai_entity e, const float *xyzw) {
    if (!g_script_ed || !xyzw) return;
    dai_quat q{ xyzw[0], xyzw[1], xyzw[2], xyzw[3] };
    dai_editor_live_set_transform(g_script_ed, (dai_node)e, nullptr, &q);
}
static dai_entity nv_find(const dai_native_api *, const char *name) {
    double id = sh_find(name, nullptr);
    return id < 0 ? (dai_entity)0 : (dai_entity)(uint32_t)id;
}
static const char *nv_name_of(const dai_native_api *, dai_entity e) {
    static char buf[64];
    dai_node_desc r{};
    if (g_scene_doc && dai_doc_get(g_scene_doc, (dai_node)e, &r) == DAI_OK)
        std::snprintf(buf, sizeof(buf), "%s", r.name);
    else buf[0] = 0;
    return buf;
}
static float nv_time(const dai_native_api *) { return g_native_time; }
static int nv_key(const dai_native_api *, uint32_t key) {
    return g_win_for_scripts ? dai_window_key_down(g_win_for_scripts, key) : 0;
}

static dai_native_api g_native_api = {
    DAI_NATIVE_ABI, nullptr,
    nv_log, nv_get_pos, nv_set_pos, nv_get_vel, nv_set_vel, nv_impulse,
    nv_get_scale, nv_set_scale, nv_get_rot, nv_set_rot,
    nv_find, nv_name_of, nv_time, nv_key
};

struct RunningNative { int id; dai_node node; std::string path; };
static std::vector<RunningNative> g_natives;

static bool is_cpp_script(const std::string &p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot + 1);
    for (char &c : e) c = (char)std::tolower((unsigned char)c);
    return e == "cpp" || e == "cc" || e == "cxx";
}""", "native api")

s = sub1(s,
"""static void scripts_stop() {
    for (RunningScript &r : g_running) dai_script_destroy(r.s);
    g_running.clear();
}""",
"""static void scripts_stop() {
    for (RunningScript &r : g_running) dai_script_destroy(r.s);
    g_running.clear();
    // Unloading the libraries on Stop is what makes editing a .cpp and
    // pressing Play again pick up the change: Windows will not replace a DLL
    // that is still mapped.
    if (g_native) for (const RunningNative &r : g_natives) dai_native_unload(g_native, r.id);
    g_natives.clear();
    g_native_time = 0.0f;
}""", "scripts_stop native")

s = sub1(s,
"""            char full[640];
            std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path.c_str());
            dai_script *s = dai_script_create(err, sizeof(err));""",
"""            char full[640];
            std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path.c_str());

            // A .cpp goes down the native path: compile, load, init. Errors
            // are the compiler's own text, in the console, where a user can
            // read them.
            if (is_cpp_script(path)) {
                if (!g_native) {
                    char cache[700];
                    std::snprintf(cache, sizeof(cache), "%s/.native", g_assets_dir);
                    g_native = dai_native_create(cache);
                }
                char nerr[1024] = { 0 };
                char incdir[700];
                std::snprintf(incdir, sizeof(incdir), "%s/../include", g_assets_dir);
                int nid = dai_native_load(g_native, full, incdir, nerr, sizeof(nerr));
                if (nid < 0) {
                    std::printf("cpp %s: %s\\n", path.c_str(), nerr);
                    if (g_panels_for_log) {
                        char line[1200];
                        std::snprintf(line, sizeof(line), "%s: %s", path.c_str(), nerr);
                        dai_editor_ui_log(g_panels_for_log, 2, line);
                    }
                } else {
                    dai_native_init(g_native, nid, &g_native_api, (dai_entity)(uint32_t)id);
                    g_natives.push_back({ nid, id, path });
                }
                continue;
            }

            dai_script *s = dai_script_create(err, sizeof(err));""", "native load")

s = sub1(s,
"""    if (!g_running.empty()) {
        std::printf("scripts: %u running\\n", (unsigned)g_running.size());""",
"""    if (!g_natives.empty()) {
        char line[96];
        std::snprintf(line, sizeof(line), "play: %u C++ behaviour(s) running",
                      (unsigned)g_natives.size());
        std::printf("%s\\n", line);
        if (g_panels_for_log) dai_editor_ui_log(g_panels_for_log, 0, line);
    }
    if (!g_running.empty()) {
        std::printf("scripts: %u running\\n", (unsigned)g_running.size());""", "native count log")

wr(P, s)
print("patch10a done")
