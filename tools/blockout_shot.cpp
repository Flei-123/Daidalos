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
    dai_render_shadow_extent(r, 20.0f);
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
