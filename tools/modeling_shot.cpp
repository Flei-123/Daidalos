// The modelling round, photographed - and the bridge, proved.
//
//   DAI_SHADER_DIR=shaders ./build/modeling_shot [OUTDIR] [W] [H] [options]
//
//     --bridge PORT     the port to open the local JSON socket on (default:
//                       the first free one from 8181). 127.0.0.1 only.
//     --script FILE     the JS that builds the room, sent down that socket
//                       (default: examples/scripts/innen_room.js)
//     --project DIR     the project whose Assets the panels browse
//     --prefix P        prefix for the file names, as droneshow_shot has
//     --serve SECONDS   do not photograph anything: open the bridge and answer
//                       until a client says {"cmd":"quit"} or the time runs
//                       out. This is what tools/daibridge.py and
//                       tools/bridge_check.sh drive.
//
// It is a HOST, not a test: it puts the same four seams together that
// examples/editor_demo.cpp does (include/dai_ext.h) and drives them headless,
// so the pictures show the real editor with the real panels rather than a
// drawing of one. The room itself is not in this file on purpose - it arrives
// over the socket as JavaScript, exactly the way Jarvis sends it.

#include "dai_editor_ui.h"
#include "dai_render.h"
#include "dai_ext.h"

#ifdef DAI_WITH_SCRIPT
#include "dai_script.h"
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---- the pieces the seams and the bridge reach back into --------------------
static dai_doc       *g_doc = nullptr;
static dai_editor    *g_ed = nullptr;
static dai_editor_ui *g_panels = nullptr;
static dai_ui       *g_ui = nullptr;
static dai_renderer  *g_renderer = nullptr;
static dai_scene     *g_scene = nullptr;
static uint32_t       g_width = 1600, g_height = 900;
static char           g_assets_dir[512] = { 0 };

#ifdef DAI_WITH_SCRIPT
// ---- the component table ----------------------------------------------------
//
// The same table examples/editor_demo.cpp answers behaviours and the inspector
// with - see the note at the top of include/dai_blockout_props.inl, which is
// written to be included by "any other host that answers component properties
// by name". This host is that other host: the bridge's JS must mean exactly
// the same thing here as it does in the editor, or a room that was built
// through the socket would not open in the editor that photographs it.
// Seam: the same component table the editor answers with -
// include/dai_props_host.inl. The bridge's JS must mean exactly the same thing
// here as it does in the editor.
#define DAI_PROPS_DOC g_doc
#include "dai_props_host.inl"
#include "dai_ui_layout.inl"

static double sh_find(const char *name, void *) {
    if (!name || !*name || !g_doc) return -1.0;
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> all(n ? n : 1);
    if (n) dai_doc_nodes(g_doc, all.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, all[i], &r) == DAI_OK && std::strcmp(r.name, name) == 0)
            return (double)(uint32_t)all[i];
    }
    return -1.0;
}
static int sh_get_pos(double id, double *xyz, void *) {
    dai_node_desc r{};
    if (!g_doc || dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return 0;
    xyz[0] = r.position.x; xyz[1] = r.position.y; xyz[2] = r.position.z;
    return 1;
}
static void sh_set_pos(double id, const double *xyz, void *) {
    dai_node_desc r{};
    if (!g_doc || dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    r.position = dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] };
    dai_doc_set(g_doc, (dai_node)(uint32_t)id, &r);
}
static int sh_get_rot(double id, double *xyzw, void *) {
    dai_node_desc r{};
    if (!g_doc || dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return 0;
    xyzw[0] = r.rotation.x; xyzw[1] = r.rotation.y;
    xyzw[2] = r.rotation.z; xyzw[3] = r.rotation.w;
    return 1;
}
static void sh_set_rot(double id, const double *xyzw, void *) {
    dai_node_desc r{};
    if (!g_doc || dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    r.rotation = dai_quat{ (float)xyzw[0], (float)xyzw[1], (float)xyzw[2], (float)xyzw[3] };
    dai_doc_set(g_doc, (dai_node)(uint32_t)id, &r);
}
static void sh_set_text(double id, const char *s, void *) {
    dai_node_desc r{};
    if (!g_doc || dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    std::snprintf(r.text, sizeof(r.text), "%s", s ? s : "");
    if (!r.text_on) r.text_on = 1;
    dai_doc_set(g_doc, (dai_node)(uint32_t)id, &r);
}
// The six component functions are the shared ones from
// include/dai_props_host.inl - this host only wraps them in the signatures
// dai_script_node_host wants, exactly as the editor does.
static double sh_get_num(double id, const char *prop, void *) {
    return comp_get_num((dai_node)(uint32_t)id, prop, 0.0);
}
static void sh_set_num(double id, const char *prop, double v, void *) {
    comp_set_num((dai_node)(uint32_t)id, prop, v);
}
static int sh_get_vec(double id, const char *prop, double *xyz, void *) {
    return comp_get_vec((dai_node)(uint32_t)id, prop, xyz);
}
static void sh_set_vec(double id, const char *prop, const double *xyz, void *) {
    comp_set_vec((dai_node)(uint32_t)id, prop, xyz);
}
static const char *sh_get_str(double id, const char *prop, void *) {
    return comp_get_str((dai_node)(uint32_t)id, prop);
}
static void sh_set_str(double id, const char *prop, const char *v, void *) {
    comp_set_str((dai_node)(uint32_t)id, prop, v);
}


// Seam: spawning - what scene.spawn()/scene.destroy() mean, shared by every
// host that runs behaviours. include/dai_spawn_host.inl.
#include "dai_spawn_host.inl"

static double sh_spawn(double src, double parent, const char *name, void *) {
    return comp_spawn((dai_node)(uint32_t)src, (dai_node)(uint32_t)parent, name);
}
static int sh_destroy(double id, void *) {
    return comp_destroy((dai_node)(uint32_t)id);
}
static double sh_child_count(double id, void *) {
    return comp_child_count((dai_node)(uint32_t)id);
}
static double sh_child_at(double id, double index, void *) {
    return comp_child_at((dai_node)(uint32_t)id, index);
}
static double sh_parent_of(double id, void *) {
    return comp_parent_of((dai_node)(uint32_t)id);
}

static dai_script_node_host g_node_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot,
                                            sh_set_text,
                                            sh_get_num, sh_set_num, sh_get_vec, sh_set_vec,
                                            sh_get_str, sh_set_str,
                                            sh_spawn, sh_destroy,
                                            sh_child_count, sh_child_at, sh_parent_of,
                                            nullptr };
#endif /* DAI_WITH_SCRIPT */

// ---- what the bridge does when it is asked for a picture --------------------
// The whole editor, panels and all - which is the difference between "the
// bridge can render" and "the bridge can show me what I just built".
// ...from the camera the BRIDGE was given. The seam's own shot_scene() reads
// daibridge::state().eye/target/fov and this host must mean the same thing by
// them, or `{"cmd":"shot","eye":[...]}` would answer with a picture taken from
// wherever the editor happened to be looking - which is how 12 and 13 came out
// byte identical and the bridge's camera looked like it worked.
static int modeling_editor_png(const char *path);
static int modeling_bridge_png(const char *path);
static void modeling_select(dai_node n);

#define DAI_BRIDGE_SHOT(path)   modeling_bridge_png(path)
#define DAI_BRIDGE_SELECT(node) modeling_select((dai_node)(node))

#include "dai_blockout_host.inl"    // module 1: blockout + CSG meshes
#include "dai_material_host.inl"    // module 2: .daimat -> dai_material, maps
#include "dai_daitex_host.inl"      // module 3: .daitex -> baked PNGs
#include "dai_bridge_host.inl"      // module 4: the local JSON socket
#include "dai_lights_host.inl"      // the Light component, as editor_demo lights it

static void modeling_select(dai_node n) {
    if (g_ed) dai_editor_select(g_ed, n, 0);
}

// The bridge asked for a picture and said where from. Both cameras are moved,
// not only the renderer's: the editor's camera is what the gizmo, the picking
// and the viewport rectangle project through, and a frame where the world is
// drawn from one eye and the gizmo from another is a frame nobody can trust.
// The editor keeps the moved camera afterwards - the bridge's `camera` command
// means "look from here", and putting the old one back would undo it.
static int modeling_bridge_png(const char *path) {
    daibridge::Bridge &b = daibridge::state();
    if (g_ed)
        dai_editor_camera(g_ed, b.eye, b.target, dai_vec3{ 0, 1, 0 }, b.fov, 0.05f, 400.0f,
                          (float)g_width, (float)g_height);
    if (g_renderer)
        dai_render_camera(g_renderer, b.eye, b.target, dai_vec3{ 0, 1, 0 }, b.fov, 0.05f, 400.0f);
    return modeling_editor_png(path);
}

// The Project panel's picture for a row. Module 3 answers for a `.daitex`
// graph and 0 for everything else, which is exactly the contract in
// include/dai_daitex_host.inl - so a browser row shows the texture the graph
// bakes rather than a coloured square with an extension on it.
static const dai_ext_host *g_ext_ptr = nullptr;

// The browser is fed a FLAT list (see collect_assets): a photograph of a
// closed folder shows nothing, and this tool has no way to open one from the
// outside. So the row's name is resolved back to the file it came from here,
// and module 3 gets the path it needs.
static std::vector<std::pair<std::string, std::string> > g_asset_paths;

static dai_texture modeling_thumb(const char *asset_path, void *) {
    if (!g_ext_ptr || !asset_path) return 0;
    for (size_t i = 0; i < g_asset_paths.size(); ++i)
        if (g_asset_paths[i].first == asset_path)
            return dai_daitex_host_thumb(g_ext_ptr, g_asset_paths[i].second.c_str());
    return dai_daitex_host_thumb(g_ext_ptr, asset_path);
}

// ---- one frame of the real editor, into a PNG -------------------------------
// The same two-pass shape tools/editor_shot.cpp uses, and for the same reason:
// the Scene panel's rectangle only exists after the dock has laid itself out,
// and the camera has to know it or the gizmo lands next to what it moves.
static int modeling_editor_png(const char *path) {
    if (!g_panels || !g_renderer || !g_scene || !g_ui) return 0;
    dai_ui *ui = g_ui;
    const float W = (float)g_width, H = (float)g_height;

    std::vector<dai_render_instance> inst(8192);
    uint32_t n = dai_scene_instances(g_scene, inst.data(), (uint32_t)inst.size(), 1.0f);
    if (n > (uint32_t)inst.size()) n = (uint32_t)inst.size();

    dai_ui_input in{};
    in.mouse_x = W * 0.5f; in.mouse_y = H * 0.5f;

    dai_ui_begin(ui, W, H, &in);
    dai_editor_ui_frame(g_panels, W, H);
    dai_ui_end(ui);
    {
        float lx = 0, ly = 0, lw = W, lh = H;
        dai_editor_ui_viewport_rect(g_panels, &lx, &ly, &lw, &lh);
        dai_editor_camera_viewport_rect(g_ed, lx, ly, lw, lh);
    }
    // M3: the strings of THIS frame, written down with the box and the clip
    // they went into. Recording is switched on for the second pass only - the
    // first pass exists to let the dock lay itself out, and a log of a layout
    // that is about to change is a log of something nobody photographed.
    dai_ui_text_record(ui, 1);
    dai_ui_begin(ui, W, H, &in);
    dai_editor_ui_frame(g_panels, W, H);
    dai_ui_end(ui);

    {
        // Next to the picture, same stem: 20-innen-zelle.png -> .layout.json.
        // A reviewer gets the frame, a test gets the numbers, and they are the
        // same frame - which is the whole point of writing it here rather than
        // in a separate pass that could disagree.
        static const char *const allowed[] = { "Add Component...", nullptr };
        std::string lp(path);
        const size_t dot = lp.rfind(".png");
        if (dot != std::string::npos) lp = lp.substr(0, dot);
        lp += ".layout.json";
        const int issues = dai_ui_layout_dump(ui, lp.c_str(), allowed);
        if (issues > 0)
            std::printf("   layout: %d issue(s) in %s\n", issues, lp.c_str());
    }
    dai_ui_text_record(ui, 0);

    const dai_ui_draw *draws = nullptr;
    uint32_t nb = dai_ui_draws(ui, &draws);
    std::vector<dai_ui_vertex> verts;
    std::vector<uint32_t> counts;
    std::vector<dai_texture> texes;
    for (uint32_t i = 0; i < nb; ++i) {
        verts.insert(verts.end(), draws[i].vertices, draws[i].vertices + draws[i].count);
        counts.push_back(draws[i].count);
        texes.push_back(draws[i].texture);
    }
    dai_render_ui(g_renderer, verts.data(), (uint32_t)verts.size(), counts.data(), texes.data(), nb);

    // The Lamp the room script placed. Collected out of the document every
    // frame, exactly as examples/editor_demo.cpp does it - the shared seam is
    // include/dai_lights_host.inl, because this host used to skip the step and
    // a room lit only by ambient made every material look like the same grey
    // mud whatever its maps said.
    dai_lights_host_collect(g_doc, g_ed, g_renderer);

    float vrx = 0, vry = 0, vrw = W, vrh = H;
    dai_editor_ui_viewport_rect(g_panels, &vrx, &vry, &vrw, &vrh);
    dai_render_world_clip(g_renderer, vrx, vry, vrw, vrh);
    {
        static float grid_xyz[84 * 2 * 3];
        uint32_t gn = dai_editor_ui_grid_lines(g_panels, grid_xyz, 84 * 2);
        dai_render_lines(g_renderer, grid_xyz, gn, 0.35f, 0.38f, 0.42f, 0.75f);
    }
    if (dai_render_frame(g_renderer, inst.data(), n) != DAI_OK) return 0;
    if (dai_render_write_png(g_renderer, path) != DAI_OK) return 0;
    std::printf("   %-34s %u instances, %u ui verts\n", path, n, (uint32_t)verts.size());
    std::fflush(stdout);
    return 1;
}

// ---- the client half: this tool talks to its own socket ---------------------
//
// It could call handle() directly - it is in the same process - and that is
// exactly why it does not. A room built through a function call would prove
// nothing about the socket, the framing or the JSON, and those are the three
// things an outside tool actually meets.
struct BridgeClient {
    int fd = -1;
    std::string in;
};

static int client_connect(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { ::close(fd); return -1; }
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static void host_pump(const dai_ext_host *ext, dai_doc_sync *sync);

// Sends one line and turns the crank until the answer comes back. The crank
// is the host's own frame: the bridge is polled once per frame and never on a
// thread of its own, so a client that waits without letting the host run is a
// client that waits for ever.
static std::string client_ask(BridgeClient &c, const std::string &line,
                              const dai_ext_host *ext, dai_doc_sync *sync) {
    std::string out = line + "\n";
    size_t sent = 0;
    for (int spin = 0; spin < 20000 && sent < out.size(); ++spin) {
        ssize_t w = ::send(c.fd, out.c_str() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return "";
        host_pump(ext, sync);
    }
    for (int spin = 0; spin < 20000; ++spin) {
        size_t nl = c.in.find('\n');
        if (nl != std::string::npos) {
            std::string answer = c.in.substr(0, nl);
            c.in.erase(0, nl + 1);
            return answer;
        }
        char buf[4096];
        ssize_t got = ::recv(c.fd, buf, sizeof(buf), 0);
        if (got > 0) { c.in.append(buf, (size_t)got); continue; }
        if (got == 0) return "";
        if (errno != EAGAIN && errno != EWOULDBLOCK) return "";
        host_pump(ext, sync);
    }
    return "";
}

static void host_pump(const dai_ext_host *ext, dai_doc_sync *sync) {
    dai_doc_sync_apply(sync);
    dai_daitex_host_poll(ext);
    dai_material_host_apply(ext);
    dai_blockout_host_sync(ext);
    dai_bridge_host_poll(ext);
}

// ---- the Project panel's file list -----------------------------------------
// The real files of the project, so the browser at the bottom of the picture
// shows what this round produced (.daitex graphs, .daimat materials) instead
// of the "nothing mounted" placeholder.
static void collect_assets(const std::string &dir, const std::string &rel,
                           std::vector<std::string> *out, int depth) {
    if (depth > 3 || out->size() > 64) return;
    DIR *d = ::opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    for (struct dirent *e = ::readdir(d); e; e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        std::string full = dir + "/" + name;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { subdirs.push_back(name); continue; }
        // The leaf name for the browser, the real relative path for whoever
        // has to open the file. Both, because the panel groups a path into a
        // folder and a folder nobody can click open is an empty panel.
        out->push_back(name);
        g_asset_paths.push_back(std::make_pair(name, rel + name));
    }
    ::closedir(d);
    // Deterministic order: readdir hands them over in whatever order the file
    // system feels like, and a screenshot that reshuffles its own file list
    // between two runs is a screenshot nobody can diff.
    std::sort(out->begin(), out->end());
    std::sort(subdirs.begin(), subdirs.end());
    for (size_t i = 0; i < subdirs.size(); ++i)
        collect_assets(dir + "/" + subdirs[i], rel + subdirs[i] + "/", out, depth + 1);
}

static dai_node find_node(const char *name) {
    if (!g_doc) return 0;
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> all(n ? n : 1);
    if (n) dai_doc_nodes(g_doc, all.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, all[i], &r) == DAI_OK && std::strcmp(r.name, name) == 0) return all[i];
    }
    return 0;
}

static std::string read_file(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    std::fclose(f);
    return out;
}

int main(int argc, char **argv) {
    std::string outdir = ".gauntlet-shots";
    std::string script = "examples/scripts/innen_room.js";
    std::string project;
    std::string prefix;
    int  want_port = 0;
    double serve_s = 0.0;

    std::vector<std::string> plain;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bridge" && i + 1 < argc)       want_port = std::atoi(argv[++i]);
        else if (a == "--script" && i + 1 < argc)  script = argv[++i];
        else if (a == "--project" && i + 1 < argc) project = argv[++i];
        else if (a == "--prefix" && i + 1 < argc)  prefix = argv[++i];
        else if (a == "--serve" && i + 1 < argc)   serve_s = std::atof(argv[++i]);
        else plain.push_back(a);
    }
    if (plain.size() > 0) outdir = plain[0];
    if (plain.size() > 1) g_width = (uint32_t)std::atoi(plain[1].c_str());
    if (plain.size() > 2) g_height = (uint32_t)std::atoi(plain[2].c_str());
    if (g_width < 320) g_width = 1600;
    if (g_height < 240) g_height = 900;

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 4096; cfg.physics_threads = 1;
    cfg.snapshot_ring = 8; cfg.seed = 42;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    g_scene = dai_scene_create(w);
    g_doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(g_doc, g_scene);

    dai_render_desc rd{};
    rd.width = g_width; rd.height = g_height; rd.msaa = 4;
    char err[256] = { 0 };
    g_renderer = dai_render_create(&rd, err, sizeof(err));
    if (!g_renderer) { std::printf("renderer failed: %s\n", err); return 1; }

    dai_font *font = dai_font_load_ui(13.0f, err, sizeof(err));
    dai_texture font_tex = 0;
    if (font) {
        uint32_t aw = 0, ah = 0;
        const uint8_t *atlas = dai_font_atlas(font, &aw, &ah);
        std::vector<uint8_t> rgba((size_t)aw * ah * 4);
        for (size_t i = 0; i < (size_t)aw * ah; ++i) {
            rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = atlas[i];
        }
        font_tex = dai_render_texture_create(g_renderer, rgba.data(), aw, ah, 0);
    }
    dai_ui *ui = dai_ui_create(font, font_tex);
    g_ui = ui;
    dai_icons *icons = dai_icons_create(16.0f);
    if (icons) {
        uint32_t iw = 0, ih = 0;
        const uint8_t *irgba = dai_icons_atlas_rgba(icons, &iw, &ih);
        if (irgba && iw && ih)
            dai_ui_set_icons(ui, icons, dai_render_texture_create(g_renderer, irgba, iw, ih, 0));
    }

    // Lit like a room and not like a landscape: one sun for shape, a lot of
    // ambient, because the interior of INNEN is a place with no sky in it.
    //
    // The sun stood almost straight up (0.35, 0.86, 0.36). A room seen from
    // outside is then five faces at grazing incidence and one lit roof: the
    // front wall and the floor slab under it came out the same near black, the
    // doorway was a dark rectangle in a dark wall, and 10-modeling-room.png
    // showed a box. The light comes over the camera's shoulder now - from the
    // -Z side, where the door is - so the wall it lights is the wall the
    // picture is OF, the floor slab keeps its own (upward) normal and reads as
    // a different surface, and the opening is a hole into a room that the Lamp
    // node lights from inside rather than a black patch on a black wall.
    dai_vec3 eye{ 5.2f, 3.4f, -7.6f }, look{ -0.4f, 1.1f, -0.4f }, up{ 0, 1, 0 };
    dai_render_camera(g_renderer, eye, look, up, 60.0f, 0.05f, 300.0f);
    dai_render_sun(g_renderer, dai_vec3{ -0.34f, 0.68f, -0.65f }, dai_vec3{ 1.0f, 0.95f, 0.88f }, 1.45f);
    dai_render_ambient(g_renderer, dai_vec3{ 0.34f, 0.38f, 0.46f }, dai_vec3{ 0.26f, 0.23f, 0.20f }, 0.62f);
    dai_render_exposure(g_renderer, 0.62f);
    dai_render_shadow_extent(g_renderer, 16.0f);
    dai_render_sky(g_renderer, 1);

    g_ed = dai_editor_create(g_doc, sync);
    dai_editor_camera(g_ed, eye, look, up, 60.0f, 0.05f, 300.0f, (float)g_width, (float)g_height);
    g_panels = dai_editor_ui_create(g_ed, ui);

    if (project.empty()) {
        const char *candidates[] = { "projects/INNEN", "projects/Untitled" };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
            struct stat st;
            if (::stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) { project = candidates[i]; break; }
        }
    }
    if (!project.empty()) std::snprintf(g_assets_dir, sizeof(g_assets_dir), "%s/assets", project.c_str());

    dai_ext_host ext{};
    ext.doc = g_doc;
    ext.sync = sync;
    ext.scene = g_scene;
    ext.renderer = g_renderer;
    ext.assets_dir = g_assets_dir;
    ext.asset_revision = 1;
    g_ext_ptr = &ext;
    dai_editor_ui_thumb_host(g_panels, modeling_thumb, nullptr);

    // ---- the bridge, opened because this host was TOLD to -------------------
    int port = 0;
    for (int p = want_port > 0 ? want_port : 8181; p < (want_port > 0 ? want_port + 1 : 8221); ++p) {
        if (dai_bridge_host_open(p)) { port = p; break; }
    }
    if (!port) {
        std::printf("modeling_shot: no free port for the bridge\n");
        return 1;
    }

    if (serve_s > 0.0) {
        // Driven from outside: answer until somebody says quit, or until the
        // clock runs out so a forgotten process cannot outlive its test.
        std::printf("modeling_shot: serving on 127.0.0.1:%d for %.0f s\n", port, serve_s);
        std::fflush(stdout);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            host_pump(&ext, sync);
            if (dai_bridge_host_quit()) break;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - t0.tv_sec) +
                             (double)(now.tv_nsec - t0.tv_nsec) * 1e-9;
            if (elapsed > serve_s) break;
            struct timespec nap = { 0, 2 * 1000 * 1000 };
            nanosleep(&nap, nullptr);
        }
        std::printf("modeling_shot: %u bridge commands answered\n", dai_bridge_host_handled());
        return 0;
    }

    // ---- the room arrives over the socket -----------------------------------
    BridgeClient client;
    for (int spin = 0; spin < 200 && client.fd < 0; ++spin) {
        host_pump(&ext, sync);
        client.fd = client_connect(port);
    }
    if (client.fd < 0) { std::printf("modeling_shot: could not reach the bridge\n"); return 1; }

    std::string code = read_file(script);
    if (code.empty()) { std::printf("modeling_shot: %s is empty or missing\n", script.c_str()); return 1; }

    // The JS goes over the wire as a JSON string, so it is quoted with the
    // bridge's own writer - the one the answers use - rather than by hand.
    std::string ask = "{\"cmd\":\"eval\",\"code\":" + daibridge::quote(code) + "}";
    std::string answer = client_ask(client, ask, &ext, sync);
    std::printf("modeling_shot: room <- %s\n", answer.c_str());
    if (answer.find("\"ok\":true") == std::string::npos) {
        std::printf("modeling_shot: the room script failed\n");
        return 1;
    }
    for (int i = 0; i < 4; ++i) host_pump(&ext, sync);
    dai_step(w);
    host_pump(&ext, sync);

    // ---- what the Project panel shows ---------------------------------------
    std::vector<std::string> assets;
    if (g_assets_dir[0]) collect_assets(g_assets_dir, "", &assets, 0);
    std::vector<const char *> asset_ptrs;
    for (size_t i = 0; i < assets.size(); ++i) asset_ptrs.push_back(assets[i].c_str());
    if (!asset_ptrs.empty())
        dai_editor_ui_asset_list(g_panels, asset_ptrs.data(), (uint32_t)asset_ptrs.size());
    std::printf("modeling_shot: project %s, %u files in the browser\n",
                project.empty() ? "(none)" : project.c_str(), (uint32_t)asset_ptrs.size());

    ::mkdir(outdir.c_str(), 0777);
    auto shot = [&](const char *name) {
        std::string path = outdir + "/" + prefix + name;
        if (!modeling_editor_png(path.c_str()))
            std::printf("   FAILED to write %s\n", path.c_str());
    };

    // 1. the room, from the doorway, with the CSG wall selected and its
    //    inspector open - the picture the round is judged on.
    //
    // "From the doorway" was a caption, not a camera: the old eye stood at
    // (5.2, 3.4, -7.6) and looked at the room's corner from above, which is a
    // picture of a closed box - the one thing this round built, the hole the
    // boolean cut, was on the far side of the wall the camera was reading.
    // The eye is at door height and off to the +X side now, and its axis goes
    // THROUGH the opening (x = 0, 1.1 m up, the outer face at z = -2.7) and on
    // into the room: the wall is seen from the front, the opening is a lit
    // interior seen through it, and the two are not the same colour.
    dai_node wall = find_node("Wall.Front");
    if (wall) dai_editor_select(g_ed, wall, 0);
    dai_editor_gizmo_mode(g_ed, DAI_GIZMO_TRANSLATE);
    // The green wireframe is the SELECTION's collider, and the wall arrived
    // from the script with the default 1 m box on it - a metre cube standing
    // in the middle of a 1.1 x 2.05 m opening, which is exactly the part of
    // the picture the round is about. The collider is given the wall's own
    // size here: it stops covering the hole, and it stops claiming the wall
    // can only be hit in the middle.
    if (wall) {
        dai_node_desc wr{};
        dai_node_desc sl{};
        dai_node slab = find_node("Wall.Front.Slab");
        if (dai_doc_get(g_doc, wall, &wr) == DAI_OK && slab &&
            dai_doc_get(g_doc, slab, &sl) == DAI_OK &&
            sl.blockout_size.x > 0.0f && sl.blockout_size.y > 0.0f) {
            wr.half_extent = dai_vec3{ sl.blockout_size.x * 0.5f, sl.blockout_size.y * 0.5f,
                                       sl.blockout_size.z * 0.5f };
            dai_doc_set(g_doc, wall, &wr);
            host_pump(&ext, sync);
        }
    }
    //
    // And CLOSE, which is the second half of the same problem: the selected
    // wall carries a gizmo and a green collider wireframe, both drawn at a
    // fixed size in PIXELS around the node's origin - and that origin sits in
    // the middle of the opening. From six metres away the hole was the same
    // size on screen as the arrows standing in it and the picture showed a
    // gizmo with some dark around it. From two metres the opening is most of
    // the frame and the gizmo is a small cross inside it: the hole is read as
    // a hole, and the room behind it as a room.
    {
        // Two metres was right while the walls were untextured grey. With the
        // maps on them a wall that fills the frame is a picture OF a wall, so
        // the camera stands back far enough that the opening, the wall around
        // it and the lit room behind are all in the same frame.
        dai_vec3 e1{ 2.75f, 1.75f, -6.60f }, l1{ -0.15f, 1.05f, -1.40f };
        dai_editor_camera(g_ed, e1, l1, up, 60.0f, 0.05f, 300.0f, (float)g_width, (float)g_height);
        dai_render_camera(g_renderer, e1, l1, up, 60.0f, 0.05f, 300.0f);
    }
    shot("10-modeling-room.png");

    // 2. the door socket: selected, so its gizmo and its fields are what the
    //    inspector shows.
    dai_node socket = find_node("DoorSocket.Front");
    if (socket) dai_editor_select(g_ed, socket, 0);
    {
        dai_vec3 e2{ 1.9f, 1.6f, -5.2f }, l2{ 0.0f, 1.05f, -2.6f };
        dai_editor_camera(g_ed, e2, l2, up, 55.0f, 0.05f, 300.0f, (float)g_width, (float)g_height);
        dai_render_camera(g_renderer, e2, l2, up, 55.0f, 0.05f, 300.0f);
    }
    shot("11-modeling-doorsocket.png");

    // 3. inside the room, looking at floor and wall together, so the two
    //    materials are in one picture and can be told apart.
    {
        dai_vec3 e3{ -2.2f, 1.60f, -1.9f }, l3{ 1.8f, 0.55f, 2.3f };
        dai_editor_camera(g_ed, e3, l3, up, 65.0f, 0.05f, 300.0f, (float)g_width, (float)g_height);
        dai_render_camera(g_renderer, e3, l3, up, 65.0f, 0.05f, 300.0f);
    }
    dai_node floor = find_node("Floor");
    if (floor) dai_editor_select(g_ed, floor, 0);
    shot("12-modeling-materials.png");

    // 4. the same scene through the BRIDGE's own shot command, with the camera
    //    it was given - the proof that "take a picture from here" works over
    //    the socket and not only from C++.
    {
        std::string path = outdir + "/" + prefix + "13-modeling-bridge-shot.png";
        std::string cmd = "{\"cmd\":\"shot\",\"path\":" + daibridge::quote(path) +
                          ",\"eye\":[5.4,3.2,-6.8],\"target\":[0,1.2,0],\"fov\":52}";
        std::string a = client_ask(client, cmd, &ext, sync);
        std::printf("modeling_shot: shot <- %s\n", a.c_str());
    }

    // The scene the room ended up as, on disk next to the pictures: a room
    // that cannot be reopened is a screenshot, not a scene.
    {
        std::string path = outdir + "/" + prefix + "modeling-room.daiscene";
        std::string cmd = "{\"cmd\":\"save\",\"path\":" + daibridge::quote(path) + "}";
        std::string a = client_ask(client, cmd, &ext, sync);
        std::printf("modeling_shot: save <- %s\n", a.c_str());
    }

    std::printf("modeling_shot: %u bridge commands answered, %u nodes in the document\n",
                dai_bridge_host_handled(), dai_doc_count(g_doc));

    ::close(client.fd);
    dai_editor_ui_destroy(g_panels);
    dai_editor_destroy(g_ed);
    dai_ui_destroy(ui);
    if (icons) dai_icons_free(icons);
    if (font) dai_font_free(font);
    dai_render_destroy(g_renderer);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(g_doc);
    dai_scene_destroy(g_scene);
    dai_destroy(w);
    return 0;
}
