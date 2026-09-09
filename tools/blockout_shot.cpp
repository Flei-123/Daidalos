// The blockout round's pictures: a room built through dai_doc in C++ - the
// way tools/editor_shot.cpp builds its crates - drawn by the real editor with
// the blockout host attached, so what is in the picture is what the host
// builds and not what a fallback scene looks like.
//
//   DAI_SHADER_DIR=shaders ./build/blockout_shot OUTDIR [W] [H] [--prefix P]
//
// Writes PREFIX14-blockout-room.png (the judged picture: four box walls, one
// of them a CSG node with a door hole, stairs, arch, the DoorSocket gizmo on
// the door, hierarchy open, inspector on the CSG node) and
// PREFIX15-blockout-doorsocket.png (the socket's node selected, close to the
// door, gizmo bright). Exit 1 on any failure - tools/run_tests.sh reads it.

#include "dai_editor_ui.h"
#include "dai_render.h"
#include "dai_ext.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

// Module 1's host seam: document -> solid -> mesh -> renderer, once per frame
// after dai_doc_sync_apply. Included, not copied - the picture must come out
// of the same code the editor runs.
#include "dai_blockout_host.inl"

static dai_doc *g_doc = nullptr;

// One node, fields set by hand: a room part is six numbers and a name.
static dai_node_desc part(const char *name, dai_vec3 pos, int kind, dai_vec3 size,
                          dai_vec3 pivot, dai_vec3 color) {
    dai_node_desc r = dai_node_desc_default();
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    // Architecture: drawn, not simulated. no_body keeps the physics out, and
    // render_extent 1,1,1 says "the mesh is already in metres" - see
    // dai_blockout_host.inl.
    r.no_body = r.no_collider = r.no_rigidbody = 1;
    r.position = pos;
    r.blockout = kind;
    r.blockout_size = size;
    r.blockout_pivot = pivot;
    r.render_extent = dai_vec3{ 1, 1, 1 };
    r.color = color;
    r.roughness = 0.85f;
    return r;
}

// By value on purpose: the callers hand over a temporary.
static dai_node add_part(dai_doc *d, dai_node_desc r) { return dai_doc_add(d, &r); }

static dai_node find_node(const char *name) {
    uint32_t n = dai_doc_count(g_doc);
    std::vector<dai_node> ids(n ? n : 1);
    dai_doc_nodes(g_doc, ids.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(g_doc, ids[i], &r) == DAI_OK && !std::strcmp(r.name, name)) return ids[i];
    }
    return 0;
}

int main(int argc, char **argv) {
    std::string outdir = "/tmp", prefix;
    uint32_t W = 1600, H = 900;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--prefix" && i + 1 < argc) { prefix = argv[++i]; continue; }
        if (positional == 0)      outdir = a;
        else if (positional == 1) W = (uint32_t)atoi(argv[i]);
        else if (positional == 2) H = (uint32_t)atoi(argv[i]);
        ++positional;
    }
    if (W < 16 || H < 16) { std::printf("blockout_shot: bad size %ux%u\n", W, H); return 1; }

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 1;
    cfg.snapshot_ring = 64; cfg.seed = 9;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("blockout_shot: world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    g_doc = doc;
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);

    // ---- the room: 6 x 5 m, 2.8 m high, the door in the -Z wall -------------
    const float RX = 6.0f, RZ = 5.0f, RH = 2.8f, T = 0.2f;
    const dai_vec3 on_floor{ 0, -1, 0 };            // pivot: built from the floor up
    const dai_vec3 wall_col{ 0.78f, 0.74f, 0.66f };
    add_part(doc, part("Floor", dai_vec3{ 0, 0, 0 }, DAI_BLOCKOUT_BOX,
                           dai_vec3{ RX + 2 * T, 0.2f, RZ + 2 * T }, dai_vec3{ 0, 1, 0 },
                           dai_vec3{ 0.36f, 0.34f, 0.30f }));

    // The front wall is the CSG node: its own box, minus the doorway under it,
    // and the socket the next room docks against - on the sill, facing out.
    dai_node_desc front = part("Wall.Front", dai_vec3{ 0, 0, -RZ * 0.5f }, DAI_BLOCKOUT_BOX,
                               dai_vec3{ RX + 2 * T, RH, T }, on_floor, wall_col);
    front.csg = DAI_CSG_SUBTRACT;
    front.door_socket = 1;
    front.door_offset = dai_vec3{ 0, 0, -T * 0.5f };
    front.door_normal = dai_vec3{ 0, 0, -1 };
    front.door_width = 1.1f;
    front.door_height = 2.05f;
    dai_node wall_front = dai_doc_add(doc, &front);
    {
        dai_node_desc door = part("Doorway", dai_vec3{ 0, 0, 0 }, DAI_BLOCKOUT_BOX,
                                  dai_vec3{ 1.1f, 2.05f, 0.6f }, on_floor, wall_col);
        door.parent = wall_front;
        dai_doc_add(doc, &door);
    }
    add_part(doc, part("Wall.Back", dai_vec3{ 0, 0, RZ * 0.5f }, DAI_BLOCKOUT_BOX,
                           dai_vec3{ RX + 2 * T, RH, T }, on_floor, wall_col));
    add_part(doc, part("Wall.Left", dai_vec3{ -RX * 0.5f, 0, 0 }, DAI_BLOCKOUT_BOX,
                           dai_vec3{ T, RH, RZ }, on_floor, wall_col));
    add_part(doc, part("Wall.Right", dai_vec3{ RX * 0.5f, 0, 0 }, DAI_BLOCKOUT_BOX,
                           dai_vec3{ T, RH, RZ }, on_floor, wall_col));

    // Stairs on the right, rising towards the back so their risers face the
    // door; an arch standing across the room on the left.
    {
        dai_node_desc st = part("Stairs", dai_vec3{ 1.9f, 0, 0.7f }, DAI_BLOCKOUT_STAIRS,
                                dai_vec3{ 1.4f, 2.2f, 2.4f }, on_floor,
                                dai_vec3{ 0.62f, 0.52f, 0.40f });
        st.blockout_steps = 6;
        dai_doc_add(doc, &st);
        dai_node_desc ar = part("Arch", dai_vec3{ -0.9f, 0, 0.9f }, DAI_BLOCKOUT_ARCH,
                                dai_vec3{ 2.4f, 2.2f, 0.4f }, on_floor,
                                dai_vec3{ 0.55f, 0.60f, 0.70f });
        ar.blockout_segments = 16;
        ar.blockout_thickness = 0.3f;
        dai_doc_add(doc, &ar);
    }
    dai_doc_sync_apply(sync);
    dai_step(w);

    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 4;
    // A still has all the time in the world: 4096 rather than the default
    // 2048, which halves the world size of a shadow texel again. Together with
    // the 9 m extent below that is the ~4 mm texel the 18 cm treads of the
    // arrayed stair need to stop shadowing themselves.
    rd.shadow_size = 4096;
    char err[256] = { 0 };
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("blockout_shot: renderer failed: %s\n", err); return 1; }

    dai_font *font = dai_font_load_ui(13.0f, err, sizeof(err));
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

    dai_render_sun(r, dai_vec3{ 0.35f, 0.80f, -0.48f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.42f);
    dai_render_exposure(r, 0.45f);
    // The shadow map covers the ROOM, not a field. 20 m of radius over a 6 x 5
    // room spread the near cascade so thin that one texel was several
    // centimetres of world - and the depth bias in shaders/mesh.frag is a
    // LENGTH scaled by exactly that texel, so on the 18 cm treads of the
    // arrayed stair the bias was deeper than the tread is thick: every second
    // step shadowed itself in blue blocks (17-modifier-array-stair.png, last
    // round). 9 m still contains the whole room and its walls with room to
    // spare, and one texel is then under a centimetre - finer than the
    // smallest thing in the picture, which is what a bias derived from the
    // texel needs to be right.
    dai_render_shadow_extent(r, 9.0f);
    dai_render_sky(r, 1);

    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);

    // The host seam: every frame, after the sync, the blockout meshes.
    dai_ext_host ext{};
    ext.doc = doc; ext.sync = sync; ext.scene = sc; ext.renderer = r;
    ext.assets_dir = ""; ext.asset_revision = 1;
    auto pump = [&]() {
        dai_doc_sync_apply(sync);
        dai_blockout_host_sync(&ext);
    };
    pump();

    {
        static const char *const ASSETS[] = {
            "blender_scene.glb", "parented.gltf", "skinned.glb",
            "Grid.png",          "ui.png",        "bindings.cfg"
        };
        dai_editor_ui_asset_list(panels, ASSETS, (uint32_t)(sizeof(ASSETS) / sizeof(ASSETS[0])));
    }

    // One frame of the real editor into a PNG - the two pass shape of
    // tools/editor_shot.cpp: the Scene panel's rect only exists once the dock
    // has laid itself out, and the camera has to know it or the gizmo lands
    // next to the wall it moves.
    dai_vec3 up{ 0, 1, 0 };
    auto shot = [&](const char *name, dai_vec3 eye, dai_vec3 look, float fov) -> int {
        pump();
        dai_editor_camera(ed, eye, look, up, fov, 0.05f, 300.0f, (float)W, (float)H);
        dai_render_camera(r, eye, look, up, fov, 0.05f, 300.0f);
        dai_editor_gizmo_hover(ed, -1000.0f, -1000.0f);

        std::vector<dai_render_instance> inst(1024);
        uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 1.0f);
        if (n > (uint32_t)inst.size()) n = (uint32_t)inst.size();

        dai_ui_input in{};
        in.mouse_x = (float)W * 0.5f; in.mouse_y = (float)H * 0.5f;
        dai_ui_begin(ui, (float)W, (float)H, &in);
        dai_editor_ui_frame(panels, (float)W, (float)H);
        dai_ui_end(ui);
        {
            float lx = 0, ly = 0, lw = (float)W, lh = (float)H;
            dai_editor_ui_viewport_rect(panels, &lx, &ly, &lw, &lh);
            dai_editor_camera_viewport_rect(ed, lx, ly, lw, lh);
        }
        dai_ui_begin(ui, (float)W, (float)H, &in);
        dai_editor_ui_frame(panels, (float)W, (float)H);
        dai_ui_end(ui);

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
        float vrx = 0, vry = 0, vrw = (float)W, vrh = (float)H;
        dai_editor_ui_viewport_rect(panels, &vrx, &vry, &vrw, &vrh);
        dai_render_world_clip(r, vrx, vry, vrw, vrh);
        {
            static float grid_xyz[84 * 2 * 3];
            uint32_t gn = dai_editor_ui_grid_lines(panels, grid_xyz, 84 * 2);
            dai_render_lines(r, grid_xyz, gn, 0.35f, 0.38f, 0.42f, 0.75f);
        }
        std::string path = outdir + "/" + prefix + name;
        if (dai_render_frame(r, inst.data(), n) != DAI_OK) {
            std::printf("blockout_shot: render failed for %s\n", path.c_str());
            return 0;
        }
        if (dai_render_write_png(r, path.c_str()) != DAI_OK) {
            std::printf("blockout_shot: could not write %s\n", path.c_str());
            return 0;
        }
        std::printf("   %-40s %u instances, %u ui verts, viewport %.0fx%.0f (frame %ux%u)\n",
                    path.c_str(), n, (uint32_t)verts.size(), (double)vrw, (double)vrh, W, H);
        std::fflush(stdout);
        return 1;
    };

    // The Inspector scrolls, and at 1100x700 the selected wall's last
    // component - Door Socket, with its Width and Height - starts below the
    // fold. A picture of a door socket without its two numbers in it is a
    // picture of a door socket that appears to have none, so the panel is
    // scrolled the way a reader would scroll it (the wheel, over the panel)
    // until the last field is inside the panel's rectangle, and then it is
    // ASSERTED - a shot that still cuts the field turns this tool red rather
    // than being written out and argued about at review.
    auto reveal_inspector_tail = [&]() -> int {
        float px = 0, py = 0, pw = 0, ph = 0;
        for (int i = 0; i < 60; ++i) {
            dai_ui_input in{};
            in.mouse_x = (float)W * 0.5f; in.mouse_y = (float)H * 0.5f;
            if (i > 0) {
                // Over the Inspector, one notch at a time: the region only
                // takes the wheel when the pointer is inside it, and its
                // scroll limit is last frame's - so this is a loop and not a
                // single jump.
                in.mouse_x = px + pw * 0.5f;
                in.mouse_y = py + ph * 0.5f;
                in.wheel = -1.0f;
            }
            dai_ui_begin(ui, (float)W, (float)H, &in);
            dai_editor_ui_frame(panels, (float)W, (float)H);
            dai_ui_end(ui);
            float fx = 0, fy = 0, fw = 0, fh = 0;
            int inside = dai_editor_ui_inspector_last_field(panels, &fx, &fy, &fw, &fh,
                                                            &px, &py, &pw, &ph);
            if (inside) return 1;
            if (pw <= 0.0f || ph <= 0.0f) return 0;
        }
        return 0;
    };
    auto assert_inspector_tail = [&](const char *what) -> int {
        float fx = 0, fy = 0, fw = 0, fh = 0, px = 0, py = 0, pw = 0, ph = 0;
        int inside = dai_editor_ui_inspector_last_field(panels, &fx, &fy, &fw, &fh,
                                                        &px, &py, &pw, &ph);
        if (inside) {
            std::printf("   %-40s last inspector field %.0f,%.0f %.0fx%.0f inside panel "
                        "%.0f,%.0f %.0fx%.0f\n", what, (double)fx, (double)fy, (double)fw,
                        (double)fh, (double)px, (double)py, (double)pw, (double)ph);
            return 1;
        }
        std::printf("blockout_shot: %s CUTS the last inspector field: field %.0f,%.0f %.0fx%.0f, "
                    "panel %.0f,%.0f %.0fx%.0f\n", what, (double)fx, (double)fy, (double)fw,
                    (double)fh, (double)px, (double)py, (double)pw, (double)ph);
        return 0;
    };

    // ---- is the subject actually IN the picture? ---------------------------
    //
    // Shot 16 went out twice with the two blocks in the far corner behind the
    // arch, because "the eye sits on their midline and backs off 4.5 m" is a
    // sentence and not a measurement. This is the measurement: the world box
    // of every subject is projected with the camera the shot was taken with,
    // and the shot is red unless the whole box lands inside the rectangle the
    // dock gave the viewport - and unless nothing else in the document stands
    // between the camera and it.
    struct AABB { dai_vec3 lo, hi; };
    // A blockout part's world box: pivot -1 means "built from this face up",
    // which is the same rule dai_blockout_host.inl applies to the mesh.
    auto desc_box = [](const dai_node_desc &r) -> AABB {
        const float px2 = r.blockout_pivot.x, py2 = r.blockout_pivot.y, pz2 = r.blockout_pivot.z;
        dai_vec3 c{ r.position.x - px2 * r.blockout_size.x * 0.5f,
                    r.position.y - py2 * r.blockout_size.y * 0.5f,
                    r.position.z - pz2 * r.blockout_size.z * 0.5f };
        AABB b;
        b.lo = dai_vec3{ c.x - r.blockout_size.x * 0.5f, c.y - r.blockout_size.y * 0.5f,
                         c.z - r.blockout_size.z * 0.5f };
        b.hi = dai_vec3{ c.x + r.blockout_size.x * 0.5f, c.y + r.blockout_size.y * 0.5f,
                         c.z + r.blockout_size.z * 0.5f };
        return b;
    };
    // The screen box of a world box: all eight corners, every one of which has
    // to be in front of the camera. Returns 0 when one is not - a subject with
    // a corner behind the eye is a subject the frame is cutting.
    auto screen_box = [&](const AABB &b, float *x0, float *y0, float *x1, float *y1) -> int {
        int first = 1;
        for (int i = 0; i < 8; ++i) {
            dai_vec3 p{ (i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y,
                        (i & 4) ? b.hi.z : b.lo.z };
            float sx = 0, sy = 0;
            if (!dai_editor_project(ed, p, &sx, &sy)) return 0;
            if (first) { *x0 = *x1 = sx; *y0 = *y1 = sy; first = 0; }
            else {
                if (sx < *x0) *x0 = sx;
                if (sx > *x1) *x1 = sx;
                if (sy < *y0) *y0 = sy;
                if (sy > *y1) *y1 = sy;
            }
        }
        return 1;
    };
    auto frame_check = [&](const char *what, const AABB *subj, int nsubj, dai_vec3 eye,
                           dai_node skip) -> int {
        float vx = 0, vy = 0, vw = (float)W, vh = (float)H;
        dai_editor_ui_viewport_rect(panels, &vx, &vy, &vw, &vh);
        float ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;      // the union, for the log
        float top_z = -1e30f, low_y = 1e30f;
        int good = 1;
        for (int s = 0; s < nsubj; ++s) {
            float x0, y0, x1, y1;
            if (!screen_box(subj[s], &x0, &y0, &x1, &y1)) {
                std::printf("blockout_shot: %s has a subject corner behind the camera\n", what);
                return 0;
            }
            if (!s) { ux0 = x0; uy0 = y0; ux1 = x1; uy1 = y1; }
            else {
                if (x0 < ux0) ux0 = x0;
                if (y0 < uy0) uy0 = y0;
                if (x1 > ux1) ux1 = x1;
                if (y1 > uy1) uy1 = y1;
            }
            if (subj[s].hi.z > top_z) top_z = subj[s].hi.z;
            if (subj[s].lo.y < low_y) low_y = subj[s].lo.y;
            if (x0 < vx || y0 < vy || x1 > vx + vw || y1 > vy + vh) {
                std::printf("blockout_shot: %s CUTS a subject: screen box %.0f,%.0f..%.0f,%.0f "
                            "outside viewport %.0f,%.0f %.0fx%.0f\n", what, (double)x0,
                            (double)y0, (double)x1, (double)y1, (double)vx, (double)vy,
                            (double)vw, (double)vh);
                good = 0;
            }
        }
        // ...and nothing standing in the way. Not "does another box overlap the
        // subject on screen" - a wall the camera looks OVER overlaps it on
        // screen and hides nothing - but the thing the sentence actually
        // means: the sight line from the eye to each corner of the subject,
        // against every other part's box. If one of the nine lines is
        // interrupted, something is in front of the subject, and the shot is
        // red rather than written out and argued about at review.
        (void)low_y; (void)top_z; (void)ux0; (void)uy0; (void)ux1; (void)uy1;
        uint32_t nn = dai_doc_count(doc);
        std::vector<dai_node> ids(nn ? nn : 1);
        dai_doc_nodes(doc, ids.data(), nn);
        // The slab test, in the form that answers "between the two": a hit
        // counts only when it happens before the far end of the segment and
        // after its start, both by a millimetre, so a box the subject TOUCHES
        // is not a box that hides it.
        auto blocks = [](const AABB &b, dai_vec3 a, dai_vec3 e) -> int {
            float t0 = 0.0f, t1 = 1.0f;
            const float A[3] = { a.x, a.y, a.z }, E[3] = { e.x, e.y, e.z };
            const float L[3] = { b.lo.x, b.lo.y, b.lo.z }, Hh[3] = { b.hi.x, b.hi.y, b.hi.z };
            for (int k = 0; k < 3; ++k) {
                float d = E[k] - A[k];
                if (std::fabs(d) < 1e-9f) {
                    if (A[k] < L[k] || A[k] > Hh[k]) return 0;
                    continue;
                }
                float ta = (L[k] - A[k]) / d, tb = (Hh[k] - A[k]) / d;
                if (ta > tb) { float s = ta; ta = tb; tb = s; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
                if (t0 > t1) return 0;
            }
            return (t1 > 0.002f && t0 < 0.998f);
        };
        for (uint32_t i = 0; i < nn && good; ++i) {
            if (ids[i] == skip) continue;       // the subject's own node
            dai_node_desc r{};
            if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;
            if (r.blockout == DAI_BLOCKOUT_NONE) continue;
            // A CSG operand is a HOLE, not a solid: the Doorway box under
            // Wall.Front is the shape the wall is missing, and nothing in the
            // picture stands where it is. It hides nothing, so it is not an
            // occluder.
            if (r.parent) {
                dai_node_desc par{};
                if (dai_doc_get(doc, r.parent, &par) == DAI_OK && par.csg != DAI_CSG_NONE)
                    continue;
            }
            AABB b = desc_box(r);
            int is_subject = 0;
            for (int s = 0; s < nsubj; ++s)
                if (b.lo.x == subj[s].lo.x && b.lo.y == subj[s].lo.y && b.lo.z == subj[s].lo.z)
                    is_subject = 1;
            if (is_subject) continue;
            for (int s = 0; s < nsubj && good; ++s) {
                for (int c = 0; c < 9; ++c) {
                    dai_vec3 p;
                    if (c == 8) p = dai_vec3{ (subj[s].lo.x + subj[s].hi.x) * 0.5f,
                                              (subj[s].lo.y + subj[s].hi.y) * 0.5f,
                                              (subj[s].lo.z + subj[s].hi.z) * 0.5f };
                    else p = dai_vec3{ (c & 1) ? subj[s].hi.x : subj[s].lo.x,
                                       (c & 2) ? subj[s].hi.y : subj[s].lo.y,
                                       (c & 4) ? subj[s].hi.z : subj[s].lo.z };
                    if (!blocks(b, eye, p)) continue;
                    std::printf("blockout_shot: %s has %s (x %.2f..%.2f y %.2f..%.2f "
                                "z %.2f..%.2f) between the camera and the subject's corner "
                                "%.2f,%.2f,%.2f\n", what, r.name, (double)b.lo.x, (double)b.hi.x,
                                (double)b.lo.y, (double)b.hi.y, (double)b.lo.z, (double)b.hi.z,
                                (double)p.x, (double)p.y, (double)p.z);
                    good = 0;
                    break;
                }
            }
        }
        if (good)
            std::printf("   %-40s subject on screen %.0f,%.0f..%.0f,%.0f inside viewport "
                        "%.0f,%.0f %.0fx%.0f, nothing in front of it\n", what, (double)ux0,
                        (double)uy0, (double)ux1, (double)uy1, (double)vx, (double)vy,
                        (double)vw, (double)vh);
        return good;
    };

    // Does this box stand in a place of its own? Every other blockout part in
    // the document is walked against it, and an overlap of more than a
    // millimetre on all three axes turns the tool red with both names and the
    // overlapping range printed.
    //
    // It exists because "the stair goes over there" was a comment, and a
    // comment does not notice when somebody moves the staircase it was written
    // about: the arrayed flight and the room's own stairs shared a cubic metre
    // for a whole round, and the picture of the Array modifier was a picture of
    // two solids z-fighting. `skip` is the node the box belongs to - a part
    // does not intersect itself.
    auto clear_check = [&](const char *what, const AABB &b, dai_node skip) -> int {
        uint32_t nn = dai_doc_count(doc);
        std::vector<dai_node> ids(nn ? nn : 1);
        dai_doc_nodes(doc, ids.data(), nn);
        int good = 1;
        for (uint32_t i = 0; i < nn; ++i) {
            if (ids[i] == skip) continue;
            dai_node_desc r{};
            if (dai_doc_get(doc, ids[i], &r) != DAI_OK) continue;
            if (r.blockout == DAI_BLOCKOUT_NONE) continue;
            if (std::strcmp(r.name, "Floor") == 0) continue;   // everything stands ON it
            AABB o = desc_box(r);
            const float M = 0.001f;
            float ox = std::min(b.hi.x, o.hi.x) - std::max(b.lo.x, o.lo.x);
            float oy = std::min(b.hi.y, o.hi.y) - std::max(b.lo.y, o.lo.y);
            float oz = std::min(b.hi.z, o.hi.z) - std::max(b.lo.z, o.lo.z);
            if (ox <= M || oy <= M || oz <= M) continue;
            std::printf("blockout_shot: %s (x %.2f..%.2f z %.2f..%.2f) runs THROUGH %s "
                        "(x %.2f..%.2f z %.2f..%.2f): overlap %.2f x %.2f x %.2f m\n",
                        what, (double)b.lo.x, (double)b.hi.x, (double)b.lo.z, (double)b.hi.z,
                        r.name, (double)o.lo.x, (double)o.hi.x, (double)o.lo.z, (double)o.hi.z,
                        (double)ox, (double)oy, (double)oz);
            good = 0;
        }
        if (good)
            std::printf("   %-40s stands clear of every other part\n", what);
        return good;
    };

    ::mkdir(outdir.c_str(), 0777);
    int ok = 1;
    // No layout of its own: the pictures come out of the editor's DEFAULT
    // layout (dai_editor_ui_layout_reset), the one a user opens - a picture
    // taken through a layout the tool brought along would prove the tool's
    // layout, not the editor's. The default keeps the inspector a full height
    // column, which is what lets five components fit at 700 px.

    // 14. the room from in front of the door wall, a little above it: the hole
    //     in the middle, the stairs and the arch over the wall, the CSG wall
    //     selected so the inspector shows Blockout, CSG and Door Socket.
    dai_editor_select(ed, wall_front, 0);
    dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);
    ok &= shot("14-blockout-room.png", dai_vec3{ 3.4f, 8.2f, -8.2f }, dai_vec3{ 0.0f, 0.2f, 0.9f }, 50.0f);

    // 15. the socket: the node that carries it selected, close to the door,
    //     the frame and the arrow bright.
    dai_node socket_node = find_node("Wall.Front");
    if (socket_node) dai_editor_select(ed, socket_node, 0);
    ok &= reveal_inspector_tail();
    ok &= shot("15-blockout-doorsocket.png", dai_vec3{ 2.4f, 2.1f, -5.6f }, dai_vec3{ 0.0f, 1.0f, -2.5f }, 50.0f);
    ok &= assert_inspector_tail("15-blockout-doorsocket.png");

    // ---- 16..18: the modifier stack ---------------------------------------
    // Added AFTER the two pictures above are taken, so those two are exactly
    // the room they always were. Every piece here is one blockout shape plus a
    // short list of rules - no vertex was placed by hand for any of them, and
    // the inspector in each picture is open on the node whose stack built it.
    {
        auto with_stack = [&](dai_node_desc r, int count, const dai_modifier *mods) {
            r.modifier_count = count;
            for (int i = 0; i < count && i < DAI_MODIFIER_MAX; ++i) r.modifiers[i] = mods[i];
            return dai_doc_add(doc, &r);
        };

        // The order down this block is the order the pictures are TAKEN, and
        // that is not the order they are numbered in: 17's flight of stairs
        // goes up first, is photographed, and only then do 16's two blocks
        // appear in the middle of the floor. A 1.1 m block standing between
        // the camera and the bottom of the flight is a block in the picture of
        // the flight - frame_check said so about two cuts of 17 in a row - and
        // a room is built up in the order a user builds it rather than in the
        // order the file names happen to sort.
        // 17. a STAIR out of one step: a single 1.2 x 0.18 x 0.32 m slab and an
        //     Array of eight, stepping up and back. Eight nodes' worth of
        //     staircase from one node and four numbers.
        dai_modifier arr{};
        arr.type = DAI_MOD_ARRAY; arr.count = 8;
        arr.offset = dai_vec3{ 0.0f, 0.18f, 0.32f };
        arr.axis = 1;
        //     WHERE it stands is not a matter of taste. The first cut put it
        //     at x = 2.0, which is the right hand bay - and the room's own
        //     six step staircase is already there (x 1.20..2.60, z -0.50..1.90),
        //     so the array's top four treads grew straight through it: two
        //     solids sharing the same cubic metre, z-fighting where their faces
        //     met, and a picture of a modifier that cannot be read because the
        //     thing it built is inside something else.
        //
        //     The free bay is the left one, in front of the arch. The step is
        //     0.9 m wide rather than 1.2 so the flight clears both its
        //     neighbours with a margin instead of touching them:
        //
        //       flight      x -2.85..-1.95   y 0..1.44   z -2.31..0.25
        //       Wall.Left   x -3.10..-2.90               (0.05 m clear)
        //       Block.Plain x -1.90..-0.80   z -1.95..-0.85 (0.05 m clear)
        //       Wall.Front                   z -2.60..-2.40 (0.09 m clear)
        //       Arch                         z  0.70..1.10  (0.45 m clear)
        //
        //     The numbers are asserted below, not admired here: STAIR_BOX is
        //     the whole eight step flight and clear_check walks every other
        //     part in the document against it.
        const float STEP_W = 0.9f, STEP_H = 0.18f, STEP_D = 0.32f;
        const dai_vec3 STAIR_POS{ -2.40f, 0.0f, -2.15f };
        dai_node stair = with_stack(part("Stair.Array", STAIR_POS,
                                         DAI_BLOCKOUT_BOX,
                                         dai_vec3{ STEP_W, STEP_H, STEP_D },
                                         on_floor, dai_vec3{ 0.66f, 0.56f, 0.44f }),
                                    1, &arr);
        // The box the ARRAY fills, which is not the box the node's own fields
        // describe: seven more copies, each one 0.18 m up and 0.32 m back.
        AABB stair_box;
        stair_box.lo = dai_vec3{ STAIR_POS.x - STEP_W * 0.5f, 0.0f,
                                 STAIR_POS.z - STEP_D * 0.5f };
        stair_box.hi = dai_vec3{ STAIR_POS.x + STEP_W * 0.5f,
                                 STEP_H + 7.0f * arr.offset.y,
                                 STAIR_POS.z + STEP_D * 0.5f + 7.0f * arr.offset.z };

        pump();

        // The three cameras stand INSIDE the room. The first cut of these had
        // the eye at z = -4.3, which is on the far side of the front wall
        // (z = -2.5): the picture was the outside of that wall, with the
        // subject somewhere behind it through the door hole. A camera for a
        // shot of an object is placed by where the object is, not by where the
        // room's own establishing shot happened to stand.

        // 17: the flight runs from z = -2.31 towards the back and climbs to
        //     1.44 m in eight treads. Seen from the right and above, across
        //     the run rather than along it, so every tread shows its riser and
        //     the eight are countable off the picture - which is the whole
        //     claim the Array modifier makes.
        //
        //     The eye stands in the middle of the room at 2.0 m, above the two
        //     blocks (1.1 m tall) it looks over, and the target is the middle
        //     of the flight. Nothing between the two: frame_check says so with
        //     the camera the shot was taken with, and clear_check has already
        //     said the flight is not INSIDE anything.
        dai_editor_select(ed, stair, 0);
        ok &= reveal_inspector_tail();
        //     Moved off the front wall and up: from z = -2.15 the two blocks
        //     of 16 (x -1.90..-0.80, 1.1 m tall) stood on the line to the top
        //     of the flight, which frame_check says out loud. From 2.4 m up in
        //     the middle of the room every one of the nine sight lines passes
        //     over them.
        //     ...and then further still: from anywhere in the middle of the
        //     room the flight's front bottom corner (-2.85, 0, -2.31) stands
        //     BEHIND the 1.1 m block of 16, which is what frame_check said
        //     about the second cut as well. The eye goes up to 2.6 m and
        //     forward to the front wall instead, where every one of the nine
        //     lines passes over the block's top rather than through it.
        const dai_vec3 CAM17_EYE{ -1.40f, 2.60f, -2.30f };
        const dai_vec3 CAM17_AT { -2.40f, 0.70f, -1.05f };
        ok &= clear_check("17 Stair.Array", stair_box, stair);
        ok &= shot("17-modifier-array-stair.png", CAM17_EYE, CAM17_AT, 68.0f);
        ok &= frame_check("17-modifier-array-stair.png", &stair_box, 1, CAM17_EYE, stair);
        ok &= assert_inspector_tail("17-modifier-array-stair.png");

        // 16. THE SAME SHAPE, twice, side by side: the left one raw, the right
        //     one with one Bevel entry on it. 8 cm and three segments, because
        //     a 2 mm chamfer is honest and invisible, and a picture nobody can
        //     read proves nothing.
        const dai_vec3 blk_col{ 0.70f, 0.66f, 0.62f };
        dai_node_desc plain = part("Block.Plain", dai_vec3{ -1.35f, 0, -1.4f },
                                   DAI_BLOCKOUT_BOX, dai_vec3{ 1.1f, 1.1f, 1.1f },
                                   on_floor, blk_col);
        dai_doc_add(doc, &plain);
        dai_modifier bev{};
        // 14 cm and three segments. A chamfer has to be READ off the picture,
        // not taken on trust: at 8 cm on a 1.1 m block the rounded corner is a
        // couple of pixels wide once the block is small enough that its
        // unbevelled twin fits in frame beside it, which proves nothing to
        // somebody looking at the two side by side.
        bev.type = DAI_MOD_BEVEL; bev.amount = 0.14f; bev.count = 3; bev.angle = 30.0f;
        dai_node_desc bevelled_desc = part("Block.Bevelled", dai_vec3{ 0.15f, 0, -1.4f },
                                           DAI_BLOCKOUT_BOX, dai_vec3{ 1.1f, 1.1f, 1.1f },
                                           on_floor, blk_col);
        dai_node bevelled = with_stack(bevelled_desc, 1, &bev);

        pump();

        // 16: square on to the two blocks from inside the room, close enough
        //     that the chamfer on the right hand one is a band and not a
        //     pixel, wide enough that the plain one is fully in frame beside
        //     it. The pair spans x -1.90..0.70, so the eye sits on their
        //     midline and backs off 3.1 m.
        dai_editor_select(ed, bevelled, 0);
        ok &= reveal_inspector_tail();
        //     The first two cuts stood at z = +1.7 - BEHIND the arch, which
        //     spans z 0.7..1.1 and is 2.2 m tall: the picture came out as the
        //     arch's shadowed back with two grey lumps behind it. The eye goes
        //     in FRONT of the arch instead, at z = -0.1, 1.3 m off the pair on
        //     their own midline (x = -0.60), and the framing is no longer
        //     argued about in a comment: frame_check projects both blocks with
        //     the camera the shot was taken with and turns the tool red unless
        //     the whole of both is inside the viewport with nothing between
        //     them and the eye.
        //     Square on to the pair from the middle of the room: the eye on
        //     their midline (x = -0.60) at eye height, 2.0 m off, and just in
        //     FRONT of the arch (z 0.70..1.10) rather than inside it. At 62
        //     degrees the two blocks together fill a little over half the
        //     frame's width with floor under both of them, and the nine sight
        //     lines to their corners are checked against every other part in
        //     the document below.
        const dai_vec3 CAM16_EYE{ -0.60f, 1.35f, 0.60f };
        const dai_vec3 CAM16_AT { -0.60f, 0.60f, -1.40f };
        const float    CAM16_FOV = 62.0f;
        AABB pair[2] = { desc_box(plain), desc_box(bevelled_desc) };
        ok &= shot("16-modifier-bevel.png", CAM16_EYE, CAM16_AT, CAM16_FOV);
        ok &= frame_check("16-modifier-bevel.png", pair, 2, CAM16_EYE, bevelled);
        ok &= assert_inspector_tail("16-modifier-bevel.png");

        // 16a / 16b. The same block, the same camera, the same light - the one
        //     difference between the two files is whether the Bevel entry is
        //     switched on. Side by side in one frame proves that a bevelled
        //     block and a plain block look different; two files from ONE
        //     camera proves that the MODIFIER did it, and a reader can put one
        //     over the other and see exactly which pixels moved. Switched
        //     through the document (m.off), which is the same field the
        //     inspector's checkbox and the bridge's `modifier.0.off` write.
        {
            dai_node_desc cur{};
            AABB one[1] = { desc_box(bevelled_desc) };
            //     Close in on the bevelled block alone: at 1.3 m and 34
            //     degrees a 14 cm chamfer is a band a finger wide on the
            //     screen, and the light rakes across it from the left.
            //     Straight down the bevelled block's own midline (x = 0.15),
            //     which is the one line into the pair that the bracket
            //     (x <= -0.25) does not stand on. 1.9 m off, so the 1.1 m
            //     block fills two thirds of the frame's height and the 14 cm
            //     chamfer is a band a finger wide.
            const dai_vec3 EYE_AB{ 0.15f, 1.15f, 0.50f };
            const dai_vec3 AT_AB { 0.15f, 0.55f, -1.40f };
            const float    FOV_AB = 50.0f;
            if (dai_doc_get(doc, bevelled, &cur) == DAI_OK) {
                cur.modifiers[0].off = 1;
                dai_doc_set(doc, bevelled, &cur);
            }
            pump();
            ok &= shot("16a-ohne-bevel.png", EYE_AB, AT_AB, FOV_AB);
            ok &= frame_check("16a-ohne-bevel.png", one, 1, EYE_AB, bevelled);
            if (dai_doc_get(doc, bevelled, &cur) == DAI_OK) {
                cur.modifiers[0].off = 0;
                dai_doc_set(doc, bevelled, &cur);
            }
            pump();
            ok &= shot("16b-mit-bevel.png", EYE_AB, AT_AB, FOV_AB);
            ok &= frame_check("16b-mit-bevel.png", one, 1, EYE_AB, bevelled);
        }

        // 18. MIRROR: half a bracket, modelled once. The node holds the left
        //     half only - a wedge leaning in towards the middle - and the
        //     Mirror entry puts the right half back, welded down the node's
        //     own YZ plane. The claim the picture has to carry is that the
        //     second half is not modelled anywhere: delete the entry and half
        //     the object is gone.
        //
        //     It stands in the middle bay in front of the arch, clear of 16's
        //     two blocks (z -1.95..-0.85) and of the flight in the left bay,
        //     with the eye off to the right so both halves are seen at an
        //     angle rather than edge on down the seam.
        dai_modifier mir{};
        mir.type = DAI_MOD_MIRROR; mir.axis = 0; mir.amount = 0.002f;
        dai_node_desc half = part("Bracket.Half", dai_vec3{ -0.70f, 0.0f, -0.10f },
                                  DAI_BLOCKOUT_WEDGE, dai_vec3{ 0.55f, 0.85f, 0.55f },
                                  on_floor, dai_vec3{ 0.58f, 0.60f, 0.66f });
        dai_node bracket = with_stack(half, 1, &mir);
        pump();
        dai_editor_select(ed, bracket, 0);
        ok &= reveal_inspector_tail();

        const dai_vec3 CAM18_EYE{ 1.00f, 1.60f, -0.80f };
        AABB bracket_box[1] = { desc_box(half) };
        ok &= shot("18-modifier-mirror.png", CAM18_EYE,
                   dai_vec3{ -0.70f, 0.80f, -0.10f }, 58.0f);
        ok &= frame_check("18-modifier-mirror.png", bracket_box, 1, CAM18_EYE, bracket);
        ok &= assert_inspector_tail("18-modifier-mirror.png");

        // 19. THE STACK ITSELF: one node with three entries on it - Array,
        //     Bevel, Mirror - and the middle one switched OFF. Everything the
        //     list can do is in the one picture: the type of each entry, the
        //     tick that turns it off, the arrows that move it, and the fields
        //     of each underneath. The geometry says the same thing the list
        //     does: three copies in a row (Array), no chamfer on them (Bevel,
        //     off), and the whole row again on the far side of the node's own
        //     ZX plane (Mirror on Z).
        {
            dai_modifier three[3]{};
            three[0].type = DAI_MOD_ARRAY; three[0].count = 3;
            three[0].offset = dai_vec3{ 0.65f, 0.0f, 0.0f };
            three[0].axis = 1;
            three[1].type = DAI_MOD_BEVEL; three[1].off = 1;      // the one that is OFF
            three[1].amount = 0.06f; three[1].count = 2; three[1].angle = 30.0f;
            three[2].type = DAI_MOD_MIRROR; three[2].axis = 2; three[2].amount = 0.002f;
            //     The free bay is the right hand front one: the stair array
            //     took the left (x -2.85..-1.95), the two blocks the middle
            //     (x -1.90..0.70), the room's own staircase everything at
            //     z >= -0.50 on the right. This stands in what is left -
            //     x 1.03..2.78, z -1.95..-1.25 - and clear_check says so
            //     rather than this comment. The pivot is the node's own -Z
            //     face (pivot z = -1), because a Mirror on Z about a plane
            //     that runs through the middle of the box would put the copy
            //     back where the original already is.
            dai_node_desc rail_desc = part("Rail.Stack", dai_vec3{ 1.25f, 0, -1.60f },
                                           DAI_BLOCKOUT_BOX, dai_vec3{ 0.45f, 0.55f, 0.35f },
                                           dai_vec3{ 0, -1, -1 }, dai_vec3{ 0.60f, 0.64f, 0.56f });
            dai_node rail = with_stack(rail_desc, 3, three);
            pump();
            dai_editor_select(ed, rail, 0);
            ok &= reveal_inspector_tail();
            //     What the stack builds, as a box: two more copies half a
            //     metre apart along +X, and the whole of that mirrored across
            //     the node's own z = 0 plane. Written out rather than taken
            //     from the mesh, so the frame check and the clearance check
            //     are independent statements about where the thing should be.
            AABB rb = desc_box(rail_desc);
            rb.hi.x += three[0].offset.x * (float)(three[0].count - 1);
            float mz = rail_desc.position.z;
            float lo = mz - (rb.hi.z - mz), hi = mz - (rb.lo.z - mz);
            if (lo < rb.lo.z) rb.lo.z = lo;
            if (hi > rb.hi.z) rb.hi.z = hi;
            //     From the middle of the room, in front of the bay and above
            //     it: the three copies run away to the right and their
            //     mirrored row stands behind them, which is the arrangement
            //     the two switched-on entries describe.
            const dai_vec3 EYE19{ 0.75f, 1.25f, -0.55f };
            const dai_vec3 AT19 { 1.85f, 0.30f, -1.60f };
            AABB rail_box[1] = { rb };
            ok &= clear_check("19 Rail.Stack", rb, rail);
            ok &= shot("19-modifier-stack.png", EYE19, AT19, 62.0f);
            ok &= frame_check("19-modifier-stack.png", rail_box, 1, EYE19, rail);
            ok &= assert_inspector_tail("19-modifier-stack.png");
        }
    }

    std::printf("blockout_shot: %u nodes in the document, %s\n", dai_doc_count(doc),
                ok ? "ok" : "FAILED");

    dai_editor_ui_destroy(panels);
    dai_editor_destroy(ed);
    dai_ui_destroy(ui);
    if (icons) dai_icons_free(icons);
    if (font) dai_font_free(font);
    dai_render_destroy(r);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    return ok ? 0 : 1;
}
