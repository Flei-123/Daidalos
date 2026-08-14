// Renders the drone show panels - storyboard, parameters, validation, the
// preview with its timeline and the status line - to PNGs, so the show mode
// can be LOOKED at rather than only asserted about. The sibling of
// tools/editor_shot.cpp, and it exists for the same reason: a panel that is
// green in a test and unreadable on screen is still broken.
//
//   DAI_SHADER_DIR=shaders ./build/droneshow_shot .gauntlet-shots [W] [H] [SET]
//
// SET names the picture set. Without a dash it is a PREFIX - `narrow-` writes
// `narrow-03-viewport-conflict.png` - and with a leading dash it is a suffix,
// which is how the first runs of this tool named their files. Either way the
// 1600x900 run with no SET owns the plain names, so a reviewer opening
// `03-viewport-conflict.png` always gets the default window size.
//
// The show it builds is a real one: three figures sampled off triangle soup
// through the ordinary pipeline, solved, validated - plus one figure with a
// pair of drones deliberately placed inside the minimum distance, because a
// validation panel photographed with an empty list proves nothing about the
// panel that has to show a conflict.
//
// And a real PROJECT around it. The show panels only appear because a project
// of kind droneshow is open, so the tool creates one on disk (and a game
// project beside it, which must keep opening exactly as it always did), opens
// it, exports the solved show into its assets folder and hands the editor the
// same three callbacks examples/editor_demo.cpp hands it. What the pictures
// then show is the editor with a project in it, not a panel set floating in a
// window.

#include "dai_editor_ui.h"
#include "dai_gltf.h"
#include "dai_project.h"
#include "dai_render.h"
#include "dai_show.h"
#include "dai_show_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The same triangle soup a glTF import hands over - generated here so the tool
// needs no asset on disk.
struct Soup {
    std::vector<float>    pos;
    std::vector<uint32_t> idx;
    void tri(const float *a, const float *b, const float *c) {
        const float *v[3] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            idx.push_back((uint32_t)(pos.size() / 3));
            pos.push_back(v[i][0]); pos.push_back(v[i][1]); pos.push_back(v[i][2]);
        }
    }
};

static void figure_sphere(Soup &s, int rings, int segs) {
    for (int i = 0; i < rings; ++i) {
        float p0 = (float)M_PI * (float)i / (float)rings;
        float p1 = (float)M_PI * (float)(i + 1) / (float)rings;
        for (int j = 0; j < segs; ++j) {
            float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
            float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
            float a[3] = { std::sin(p0) * std::cos(t0), std::cos(p0), std::sin(p0) * std::sin(t0) };
            float b[3] = { std::sin(p1) * std::cos(t0), std::cos(p1), std::sin(p1) * std::sin(t0) };
            float c[3] = { std::sin(p1) * std::cos(t1), std::cos(p1), std::sin(p1) * std::sin(t1) };
            float d[3] = { std::sin(p0) * std::cos(t1), std::cos(p0), std::sin(p0) * std::sin(t1) };
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

static void figure_cube(Soup &s) {
    const float h = 1.0f;
    const float v[8][3] = {
        { -h, -h, -h }, {  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h },
        { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }
    };
    const int f[12][3] = {
        {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}
    };
    for (int i = 0; i < 12; ++i) s.tri(v[f[i][0]], v[f[i][1]], v[f[i][2]]);
}

static void figure_ring(Soup &s, int segs, float thick) {
    for (int j = 0; j < segs; ++j) {
        float t0 = 2.0f * (float)M_PI * (float)j / (float)segs;
        float t1 = 2.0f * (float)M_PI * (float)(j + 1) / (float)segs;
        for (int k = 0; k < 12; ++k) {
            float u0 = 2.0f * (float)M_PI * (float)k / 12.0f;
            float u1 = 2.0f * (float)M_PI * (float)(k + 1) / 12.0f;
            auto pt = [&](float t, float u, float *o) {
                float rr = 1.0f + thick * std::cos(u);
                o[0] = rr * std::cos(t); o[1] = thick * std::sin(u); o[2] = rr * std::sin(t);
            };
            float a[3], b[3], c[3], d[3];
            pt(t0, u0, a); pt(t1, u0, b); pt(t1, u1, c); pt(t0, u1, d);
            s.tri(a, b, c); s.tri(a, c, d);
        }
    }
}

// Opens the project if this tool has run before, creates it otherwise. Both
// paths end with a project of the kind asked for: dai_project_create_kind
// refuses to overwrite, which is the behaviour that matters in an editor and
// the reason a shot tool has to ask first.
static dai_project *project_here(const char *root, const char *name, int kind) {
    char path[512], err[256] = { 0 };
    std::snprintf(path, sizeof(path), "%s/%s", root, name);
    dai_project *p = dai_project_is_valid(path) ? dai_project_open(path, err, sizeof(err))
                                                : dai_project_create_kind(root, name, kind,
                                                                          err, sizeof(err));
    if (!p) std::printf("project %s: %s\n", name, err);
    return p;
}

// The picker's feed. The editor does not know where projects live, so the tool
// that made them says so - the same contract examples/editor_demo.cpp honours.
static std::vector<std::string> g_project_names;

static const char *project_list_cb(uint32_t index, void *) {
    return index < g_project_names.size() ? g_project_names[index].c_str() : nullptr;
}

static const char *kind_word(int kind) {
    return kind == DAI_PROJECT_DRONESHOW ? "droneshow" : "game";
}

int main(int argc, char **argv) {
    std::string outdir = argc > 1 ? argv[1] : ".gauntlet-shots";
    const uint32_t W = argc > 2 ? (uint32_t)atoi(argv[2]) : 1600;
    const uint32_t H = argc > 3 ? (uint32_t)atoi(argv[3]) : 900;
    // A tag in the file names, so the same tool can photograph the same show at
    // a second window size without overwriting the first run. Narrow is where
    // labels collide, so the narrow run is the one worth keeping. `narrow-`
    // goes in front of the name, `-narrow` behind it; the empty set keeps the
    // plain names.
    std::string set = argc > 4 ? argv[4] : "";
    const bool  set_is_suffix = !set.empty() && set[0] == '-';
    auto shot_path = [&](const char *name) {
        return set_is_suffix ? outdir + "/" + name + set + ".png"
                             : outdir + "/" + set + name + ".png";
    };

    // ---- the project ------------------------------------------------------
    // One of each kind, side by side: the droneshow the panels belong to, and
    // a game project that has to keep opening the way it always did. The kinds
    // are read back off disk rather than remembered, because that read is the
    // thing the feature claims works.
    const char *proot = "build/shot_projects";
    dai_project *game = project_here(proot, "Fixture Game", DAI_PROJECT_GAME);
    dai_project *proj = project_here(proot, "Drone Show Demo", DAI_PROJECT_DRONESHOW);
    if (!proj) return 1;
    if (game) std::printf("project %-16s kind %s\n", dai_project_name(game),
                          kind_word(dai_project_kind(game)));
    std::printf("project %-16s kind %s\n", dai_project_name(proj),
                kind_word(dai_project_kind(proj)));
    if (dai_project_kind(proj) != DAI_PROJECT_DRONESHOW) {
        std::printf("the droneshow project did not come back as one\n");
        return 1;
    }

    // ---- the show ---------------------------------------------------------
    // The safety numbers go through settings/project.txt and come back out of
    // it, exactly as the editor does it: what is photographed is what a second
    // operator would read off the file, not what this tool had in mind.
    dai_project_settings ps = dai_project_settings_default();
    dai_project_settings_load(proj, &ps);
    ps.drone_count    = 420;
    ps.min_distance_m = 2.0f;
    ps.v_max_ms       = 8.0f;
    ps.a_max_ms2      = 4.0f;
    ps.takeoff_alt_m  = 30.0f;
    ps.fps            = 10;                // a shot tool, not a flight review
    ps.show_seed      = 20260814ull;
    dai_project_settings_save(proj, &ps);
    ps = dai_project_settings_default();
    dai_project_settings_load(proj, &ps);

    dai_show_settings s = dai_show_settings_default();
    s.min_distance_m       = ps.min_distance_m;
    s.v_max_ms             = ps.v_max_ms;
    s.a_max_ms2            = ps.a_max_ms2;
    s.drone_count          = ps.drone_count;
    s.show_origin_lat      = ps.show_origin_lat;
    s.show_origin_lon      = ps.show_origin_lon;
    s.show_origin_amsl     = ps.show_origin_amsl;
    s.show_orientation_deg = ps.show_orientation_deg;
    s.takeoff_alt_m        = ps.takeoff_alt_m;
    s.fps                  = ps.fps;
    s.fence_half_x         = ps.fence_half_x;
    s.fence_half_z         = ps.fence_half_z;
    s.fence_top_m          = ps.fence_top_m;
    s.min_ground_m         = ps.min_ground_m;
    s.seed                 = ps.show_seed;

    dai_show *sh = dai_show_create(&s);
    if (!sh) { std::printf("show failed\n"); return 1; }

    struct Fig { int kind; const char *name; float size; int mode; float hold; };
    const Fig figs[3] = {
        { 0, "Sphere",     70.0f, DAI_SHOW_SAMPLE_SURFACE,    4.0f },
        { 2, "Ring",       90.0f, DAI_SHOW_SAMPLE_SILHOUETTE, 4.0f },
        { 1, "Cube",       70.0f, DAI_SHOW_SAMPLE_SURFACE,    4.0f }
    };
    for (int i = 0; i < 3; ++i) {
        Soup soup;
        if (figs[i].kind == 0)      figure_sphere(soup, 24, 32);
        else if (figs[i].kind == 1) figure_cube(soup);
        else                        figure_ring(soup, 48, 0.28f);

        dai_show_sample_desc d;
        std::memset(&d, 0, sizeof(d));
        d.positions        = soup.pos.data();
        d.vertex_count     = (uint32_t)(soup.pos.size() / 3);
        d.indices          = soup.idx.data();
        d.index_count      = (uint32_t)soup.idx.size();
        d.base_rgba        = (i == 0) ? 0xFF40C0FFu : (i == 1) ? 0xFF60FF90u : 0xFFFF80D0u;
        d.mode             = figs[i].mode;
        d.count            = s.drone_count;
        d.min_distance_m   = s.min_distance_m;
        d.scale            = figs[i].size * 0.5f;
        d.centre           = dai_vec3{ 0.0f, s.takeoff_alt_m + figs[i].size * 0.6f, 0.0f };
        d.view_dir         = dai_vec3{ 0.0f, 0.0f, 1.0f };
        d.relax_iterations = 6;
        d.seed             = s.seed + (uint64_t)i * 7919ull;

        char err[256] = { 0 };
        uint32_t idx = dai_show_formation_from_mesh(sh, figs[i].name, "builtin://figure",
                                                    &d, err, sizeof(err));
        if (idx == 0xFFFFFFFFu) { std::printf("sample %s failed: %s\n", figs[i].name, err); return 1; }
        dai_show_formation_set_hold(sh, idx, figs[i].hold);
        std::printf("formation %u  %-8s %u points\n", idx, figs[i].name, s.drone_count);
    }

    // The figure that is WRONG on purpose: the sphere again, with two drones
    // pushed to 0.4 m apart. The validator has to find it, the list has to show
    // it and the preview has to paint it red - that is what these images are for.
    {
        const dai_show_point *base = dai_show_formation_points(sh, 0);
        std::vector<dai_show_point> pts(base, base + s.drone_count);
        pts[173] = pts[172];
        pts[173].x += 0.4f;
        uint32_t idx = dai_show_formation_add(sh, "Sphere (near miss)", "builtin://figure",
                                              pts.data(), (uint32_t)pts.size(), 5.0f);
        std::printf("formation %u  near miss injected between drones 172 and 173\n", idx);
    }

    char err[256] = { 0 };
    dai_result sr = dai_show_solve(sh, err, sizeof(err));
    dai_show_validate_show(sh);
    dai_show_timings tm = dai_show_get_timings(sh);
    std::printf("solve %s: assign %.1f ms, layer %.1f ms, validate %.1f ms, %u conflicts\n",
                sr == DAI_OK ? "ok" : err, tm.assign_ms, tm.layer_ms, tm.validate_ms,
                dai_show_conflict_count(sh));

    for (uint32_t i = 0; i < dai_show_conflict_count(sh) && i < 12u; ++i) {
        dai_show_conflict cf;
        if (!dai_show_conflict_at(sh, i, &cf)) continue;
        std::printf("  conflict %2u  t %7.2fs  drones %u+%u  kind %d  %.3f of %.3f m\n",
                    i, cf.time_s, cf.a, cf.b, cf.kind, cf.value, cf.limit);
    }

    // The show and its exports, written into the project they belong to, so
    // the Project panel under the viewport lists files this run really made.
    std::vector<std::string> asset_names;
    {
        char path[640];
        std::snprintf(path, sizeof(path), "%s/scenes/show.dshow", dai_project_path(proj));
        if (dai_show_save(sh, path, err, sizeof(err)) != DAI_OK)
            std::printf("save show: %s\n", err);
        const dai_show_plan *plan = dai_show_get_plan(sh);
        struct Out { const char *name; dai_result (*fn)(const dai_show_plan *,
                                                        const dai_show_settings *,
                                                        const char *, char *, size_t); };
        const Out outs[3] = { { "show.json", dai_show_export_json },
                              { "show.csv",  dai_show_export_csv },
                              { "show.skyc", dai_show_export_skyc } };
        for (int i = 0; plan && i < 3; ++i) {
            std::snprintf(path, sizeof(path), "%s/%s", dai_project_asset_dir(proj), outs[i].name);
            if (outs[i].fn(plan, &s, path, err, sizeof(err)) == DAI_OK)
                asset_names.push_back(outs[i].name);
            else
                std::printf("export %s: %s\n", outs[i].name, err);
        }
        std::printf("exported into %s: %u files\n", dai_project_asset_dir(proj),
                    (uint32_t)asset_names.size());
    }

    dai_show_ui *show = dai_show_ui_create(sh);

    // ---- the mesh the storyboard samples from -----------------------------
    // A photograph of a storyboard saying "no mesh selected - pick one in the
    // Project panel" documents a dead end, not a feature. So the tool does
    // what an operator does: it imports a real mesh off disk into the project,
    // where the Project panel below the viewport lists it, and hands the same
    // triangle soup to the panels. "From selected mesh" is then a button that
    // works in the state that was photographed.
    std::vector<float>    mesh_pos;
    std::vector<uint32_t> mesh_idx;
    std::string           mesh_asset;                 // the name in the project
    {
        const char *src = "assets/test/blender_scene.glb";
        std::vector<uint8_t> bytes;
        if (FILE *f = std::fopen(src, "rb")) {
            std::fseek(f, 0, SEEK_END);
            long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) {
                bytes.resize((size_t)n);
                if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
            }
            std::fclose(f);
        }
        dai_mesh_data md[16];
        uint32_t np = bytes.empty() ? 0u
                                    : dai_gltf_read_geometry(bytes.data(), bytes.size(), md, 16,
                                                             err, sizeof(err));
        uint32_t got = np < 16u ? np : 16u;
        for (uint32_t p = 0; p < got; ++p) {
            uint32_t base = (uint32_t)(mesh_pos.size() / 3);
            for (uint32_t v = 0; v < md[p].vertex_count; ++v) {
                mesh_pos.push_back(md[p].vertices[v].position.x);
                mesh_pos.push_back(md[p].vertices[v].position.y);
                mesh_pos.push_back(md[p].vertices[v].position.z);
            }
            for (uint32_t i = 0; i < md[p].index_count; ++i)
                mesh_idx.push_back(base + md[p].indices[i]);
        }
        if (got) dai_gltf_free_geometry(md, got);

        if (!mesh_pos.empty() && !mesh_idx.empty()) {
            // Into the mesh's own unit box, so `scale` really is "mesh units
            // to metres" and Size (m) in the panel means what it says whatever
            // the modeller worked in.
            float lo[3] = { mesh_pos[0], mesh_pos[1], mesh_pos[2] }, hi[3] = { lo[0], lo[1], lo[2] };
            for (size_t v = 0; v + 2 < mesh_pos.size(); v += 3)
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], mesh_pos[v + (size_t)k]);
                    hi[k] = std::max(hi[k], mesh_pos[v + (size_t)k]);
                }
            float ext = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
            float inv = ext > 1e-6f ? 2.0f / ext : 1.0f;
            for (size_t v = 0; v + 2 < mesh_pos.size(); v += 3)
                for (int k = 0; k < 3; ++k)
                    mesh_pos[v + (size_t)k] = (mesh_pos[v + (size_t)k] - 0.5f * (lo[k] + hi[k])) * inv;

            // The import an operator would do: the file lands in the project's
            // assets folder, which is what the Project panel reads.
            char dst[640];
            std::snprintf(dst, sizeof(dst), "%s/figure.glb", dai_project_asset_dir(proj));
            if (FILE *o = std::fopen(dst, "wb")) {
                std::fwrite(bytes.data(), 1, bytes.size(), o);
                std::fclose(o);
                mesh_asset = "figure.glb";
                asset_names.push_back(mesh_asset);
            }

            dai_show_sample_desc md_desc;
            std::memset(&md_desc, 0, sizeof(md_desc));
            md_desc.positions      = mesh_pos.data();
            md_desc.vertex_count   = (uint32_t)(mesh_pos.size() / 3);
            md_desc.indices        = mesh_idx.data();
            md_desc.index_count    = (uint32_t)mesh_idx.size();
            md_desc.base_rgba      = 0xFFFFC080u;
            md_desc.mode           = DAI_SHOW_SAMPLE_SURFACE;
            md_desc.scale          = 0.0f;            // the panel's Size (m) decides
            md_desc.centre         = dai_vec3{ 0.0f, s.takeoff_alt_m + 40.0f, 0.0f };
            md_desc.view_dir       = dai_vec3{ 0.0f, 0.0f, 1.0f };
            dai_show_ui_mesh(show, &md_desc,
                             mesh_asset.empty() ? src : (std::string("assets/") + mesh_asset).c_str());
            std::printf("mesh %-28s %u triangles -> selected in the storyboard\n",
                        src, (uint32_t)(mesh_idx.size() / 3));
        } else {
            std::printf("mesh %s not readable (%s) - the storyboard stays without one\n", src, err);
        }
    }

    // ---- the editor around it --------------------------------------------
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1;
    cfg.snapshot_ring = 16; cfg.seed = 9;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_doc_sync_apply(sync);
    dai_step(w);

    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 4;
    char rerr[256] = { 0 };
    dai_renderer *r = dai_render_create(&rd, rerr, sizeof(rerr));
    if (!r) { std::printf("renderer failed: %s\n", rerr); return 1; }

    dai_font *font = dai_font_load_ui(13.0f, rerr, sizeof(rerr));
    dai_texture font_tex = 0;
    if (font) {
        uint32_t aw = 0, ah = 0;
        const uint8_t *atlas = dai_font_atlas(font, &aw, &ah);
        std::vector<uint8_t> rgba((size_t)aw * ah * 4);
        for (size_t i = 0; i < (size_t)aw * ah; ++i) {
            rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = atlas[i];
        }
        font_tex = dai_render_texture_create(r, rgba.data(), aw, ah, 0);
    }
    dai_ui *ui = dai_ui_create(font, font_tex);
    dai_icons *icons = dai_icons_create(16.0f);
    if (icons) {
        uint32_t iw = 0, ih = 0;
        const uint8_t *irgba = dai_icons_atlas_rgba(icons, &iw, &ih);
        if (irgba && iw && ih)
            dai_ui_set_icons(ui, icons, dai_render_texture_create(r, irgba, iw, ih, 0));
    }

    dai_vec3 eye{ 0.0f, 60.0f, 200.0f }, look{ 0, 60.0f, 0 }, up{ 0, 1, 0 };
    dai_render_camera(r, eye, look, up, 55.0f, 0.1f, 800.0f);
    dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.2f);
    dai_render_ambient(r, dai_vec3{ 0.10f, 0.14f, 0.24f }, dai_vec3{ 0.06f, 0.06f, 0.08f }, 0.35f);
    dai_render_exposure(r, 0.45f);
    dai_render_sky(r, 0);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_camera(ed, eye, look, up, 55.0f, 0.1f, 800.0f, (float)W, (float)H);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);
    dai_editor_ui_show_host(panels, show);

    // The project half of the editor, fed exactly as the demo feeds it: the
    // picker lists what is on disk and the browser lists what the show wrote.
    {
        char names[16][DAI_PROJECT_NAME_MAX];
        uint32_t pn = dai_project_list(proot, names[0], 16, DAI_PROJECT_NAME_MAX);
        g_project_names.clear();
        for (uint32_t i = 0; i < pn && i < 16u; ++i) g_project_names.push_back(names[i]);
        dai_editor_ui_project_host(panels, project_list_cb, nullptr, nullptr, nullptr);
        dai_editor_ui_projects_refresh(panels);
    }
    std::vector<const char *> asset_ptrs;
    for (size_t i = 0; i < asset_names.size(); ++i) asset_ptrs.push_back(asset_names[i].c_str());
    dai_editor_ui_asset_list(panels, asset_ptrs.data(), (uint32_t)asset_ptrs.size());

    // One frame of the whole editor. `mx/my/down` are the pointer, so a hover
    // or a click can be photographed as well as a resting screen.
    auto frame = [&](float mx, float my, int down) {
        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, (float)W, (float)H, &in);
        dai_editor_ui_frame(panels, (float)W, (float)H);
        dai_ui_end(ui);
    };
    auto flush = [&](const char *path) {
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
        dai_render_ui(r, verts.data(), (uint32_t)verts.size(), counts.data(), texes.data(), nb);
        dai_render_world_clip(r, 0, 0, 0, 0);
        dai_render_frame(r, nullptr, 0);
        dai_render_write_png(r, path);
        std::printf("%-46s %u ui vertices\n", path, (uint32_t)verts.size());
    };
    auto shot = [&](const char *name, float mx, float my, int down) {
        // Twice: the dock registers its panels on the first frame and lays them
        // out on it, and immediate mode widgets settle their scroll state one
        // frame later. The picture wanted is the settled one.
        frame(mx, my, down);
        frame(mx, my, down);
        std::string path = shot_path(name);
        flush(path.c_str());
    };

    // 1  the show as it opens: every panel, the fleet at t = 0.
    dai_show_ui_seek(show, 0.0f);
    shot("01-start", (float)W * 0.5f, (float)H * 0.5f, 0);

    // 2  mid transition - the fleet in flight between two figures, which is
    //    where the colours and the layering are actually visible.
    dai_show_ui_seek(show, 10.5f);
    shot("02-transition", (float)W * 0.5f, (float)H * 0.5f, 0);

    // 3  the conflict. Seek to the moment the validator complained about and
    //    select the drones it named, exactly as clicking the row does.
    int conflict_row = -1;
    dai_show_conflict c{};
    if (dai_show_conflict_count(sh) > 0) {
        conflict_row = (int)(dai_show_conflict_count(sh) / 2);
        dai_show_conflict_at(sh, (uint32_t)conflict_row, &c);
        std::printf("conflict %d at %.2fs between drones %u and %u (%.2f m)\n",
                    conflict_row, c.time_s, c.a, c.b, c.value);
    }
    if (conflict_row >= 0) dai_show_ui_seek(show, c.time_s);
    shot("03-viewport-conflict", (float)W * 0.5f, (float)H * 0.5f, 0);

    // ---- the panels on their own -----------------------------------------
    // Same functions the dock calls, at the size the DOCK gives them, with the
    // tab head above them - a column stays a column, a strip stays a strip.
    // Blowing a 380 px panel up to the whole frame used to produce a picture
    // that was nine tenths empty background with a button stretched across it,
    // which says nothing about the panel a director actually sees.
    const float TABH = 26.0f;
    // One panel in its dock frame: the tab head, the accent line under the
    // active tab and the border the dock draws, then the panel itself in the
    // rectangle below. `focus` is the tab that is open; an unfocused frame is
    // drawn dimmer, which is how the picture says which panel it is about.
    auto tab_panel = [&](const char *title, float px, float py, float pw, float ph, int focus,
                         void (*fn)(dai_show_ui *, dai_ui *, float, float, float, float)) {
        const dai_ui_style *st = dai_ui_style_of(ui);
        dai_ui_rect(ui, px, py, pw, TABH, st->chrome);
        float tw = dai_ui_text_width(ui, title) + 24.0f;
        dai_ui_rect(ui, px, py, tw, TABH, focus ? st->panel : st->button);
        if (focus) dai_ui_rect(ui, px, py, tw, 2.0f, st->accent);
        dai_ui_text(ui, px + 12.0f, py + (TABH - dai_ui_text_height(ui)) * 0.5f,
                    title, focus ? st->text : st->text_dim);
        dai_ui_rect(ui, px, py + TABH - 1.0f, pw, 1.0f, st->panel_border);
        fn(show, ui, px, py + TABH, pw, ph - TABH);
    };
    // The panel that is being photographed, at the size the DOCK gives it -
    // and the preview beside it, because the rest of the frame is a dock too.
    // Blowing a 380 px column up to the whole window used to produce a picture
    // that was nine tenths background with one button stretched across it,
    // which says nothing about the panel a director actually uses.
    const float M = 20.0f;                              // the frame's margin
    const float COLW = std::min(380.0f, (float)W * 0.34f);
    const float BOT  = 30.0f;                           // the status line
    auto panel_shot = [&](const char *name, int which) {
        for (int pass = 0; pass < 2; ++pass) {
            dai_ui_input in{};
            in.mouse_x = -100; in.mouse_y = -100;
            dai_ui_begin(ui, (float)W, (float)H, &in);
            const dai_ui_style *st = dai_ui_style_of(ui);
            dai_ui_rect(ui, 0.0f, 0.0f, (float)W, (float)H, st->chrome);
            float ax = M, ay = M, aw = (float)W - 2.0f * M, ah = (float)H - M - BOT - 10.0f;
            if (which == 0)
                tab_panel("Storyboard", ax, ay, COLW, ah, 1, dai_show_ui_storyboard);
            else if (which == 1)
                tab_panel("Show Parameters", ax, ay, COLW, ah, 1, dai_show_ui_parameters);
            if (which == 0 || which == 1)
                tab_panel("Preview", ax + COLW + 8.0f, ay, aw - COLW - 8.0f, ah, 0,
                          dai_show_ui_viewport);
            else if (which == 2) {
                float strip = std::min(320.0f, ah * 0.42f);
                tab_panel("Preview", ax, ay, aw, ah - strip - 8.0f, 0, dai_show_ui_viewport);
                tab_panel("Validation", ax, ay + ah - strip, aw, strip, 1, dai_show_ui_validation);
            } else if (which == 3)
                tab_panel("Preview", ax, ay, aw, ah, 1, dai_show_ui_viewport);
            dai_show_ui_status(show, ui, 0.0f, (float)H - BOT, (float)W, BOT);
            dai_ui_end(ui);
        }
        flush(shot_path(name).c_str());
    };
    panel_shot("04-storyboard",   0);
    panel_shot("05-parameters",   1);
    panel_shot("06-validation",   2);
    panel_shot("07-preview-full", 3);

    // 8  the click the validation panel promises: a row is pressed, the
    //    timeline jumps to it and the two drones go red in the preview. Done
    //    through the pointer, not by setting the fields, so the picture proves
    //    the interaction rather than the state.
    {
        // The dock puts Validation across the bottom quarter of the frame, and
        // the rows begin under its tab strip, the Re-check button and the red
        // header. Rather than hard coding that stack, the pointer walks down
        // the panel until a row actually takes the click - the interface's own
        // answer (dai_show_ui_selected_conflict) is the test that it landed.
        const float TOP = 34.0f, BOTTOM = 24.0f;
        float py = TOP + 0.74f * ((float)H - TOP - BOTTOM);
        int hit = 0;
        for (float dy = 60.0f; dy < 160.0f && !hit; dy += 6.0f) {
            float rx = 120.0f, ry = py + dy;
            frame(rx, ry, 0);          // hover
            frame(rx, ry, 1);          // press
            frame(rx, ry, 0);          // release: the row commits
            frame(rx, ry, 0);
            if (dai_show_ui_selected_conflict(show) >= 0) {
                hit = 1;
                std::string path = shot_path("08-conflict-clicked");
                flush(path.c_str());
                std::printf("clicked validation row at %.0f,%.0f -> conflict %d, t=%.2f, drone %u\n",
                            rx, ry, dai_show_ui_selected_conflict(show),
                            dai_show_ui_time(show), dai_show_ui_selected_drone(show));
            }
        }
        if (!hit) std::printf("no validation row took the click\n");
    }

    dai_editor_ui_destroy(panels);
    if (game) dai_project_close(game);
    dai_project_close(proj);
    dai_editor_destroy(ed);
    dai_show_ui_destroy(show);
    dai_show_destroy(sh);
    dai_ui_destroy(ui);
    if (icons) dai_icons_free(icons);
    if (font) dai_font_free(font);
    dai_render_destroy(r);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return 0;
}
