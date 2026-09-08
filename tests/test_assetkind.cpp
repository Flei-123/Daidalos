// What KIND of file is this, as far as the scene is concerned.
//
//   ./build/test_assetkind
//
// Small on purpose, and in the suite on purpose. The bug it exists to stop is
// not subtle: a .glb dragged into the viewport fell through every branch of
// the drop - prefab? material? picture? script? - and landed in silence. No
// object, no message, nothing to report except "it does not work".
//
// The drag PREVIEW and the DROP both ask dai_editor_ui_is_placeable, so a file
// type cannot be previewable and undroppable at the same time. This is the
// test of that one answer.

#include "dai_editor_ui.h"
#include "dai_material.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    std::printf("what the scene can place\n");

    struct Case { const char *path; int want; const char *why; };
    static const Case PLACE[] = {
        { "Models/Room1.glb",       1, "a model is placed" },
        { "Models/room.GLTF",       1, "the extension is not case sensitive" },
        { "Models/chair.obj",       1, "obj is a model too" },
        { "Prefabs/Crate.daidalos", 1, "a prefab is placed" },
        { "Crate.daidalos",         1, "a prefab outside a folder is still a prefab" },
        { "Scenes/Level1.daidalos", 0, "a SCENE opens, it is not placed into itself" },
        { "scenes/Level1.daidalos", 0, "and the folder check is case insensitive" },
        { "Textures/logo.png",      0, "a picture goes on a material, not on the ground" },
        { "Textures/logo.jpg",      0, "same for a jpeg" },
        { "Materials/wood.daimat",  0, "a material is worn, not placed" },
        { "Scripts/Player.js",      0, "a script attaches" },
        { "Scripts/Player.cpp",     0, "so does a native one" },
        { "readme.md",              0, "text is text" },
        { "noextension",            0, "no extension, no guess" },
        { ".glb",                   0, "an extension with no name is not a file" },
        { "",                       0, "the empty path" },
    };
    for (const Case &c : PLACE)
        CHECK(dai_editor_ui_is_placeable(c.path) == c.want,
              "is_placeable(\"%s\") = %d, expected %d - %s",
              c.path, dai_editor_ui_is_placeable(c.path), c.want, c.why);
    CHECK(dai_editor_ui_is_placeable(nullptr) == 0, "a null path was called placeable");

    // The other half of the same question, which already had this shape: what
    // can go INTO a material or an Image field.
    static const Case TEX[] = {
        { "Textures/logo.png",      1, "png" },
        { "Textures/logo.JPG",      1, "jpeg, upper case" },
        { "Textures/logo.jpeg",     1, "jpeg, spelled out" },
        { "Textures/logo.tga",      1, "tga" },
        { "Models/Room1.glb",       0, "a model is not a texture" },
        { "Scripts/Player.js",      0, "nor is a script" },
    };
    for (const Case &c : TEX)
        CHECK(dai_editor_ui_is_texture(c.path) == c.want,
              "is_texture(\"%s\") = %d, expected %d - %s",
              c.path, dai_editor_ui_is_texture(c.path), c.want, c.why);

    // Nothing may be both: a file that is placed AND painted on is a file the
    // drop has to guess about, and guessing is what this pair exists to end.
    for (const Case &c : PLACE)
        CHECK(!(dai_editor_ui_is_placeable(c.path) && dai_editor_ui_is_texture(c.path)),
              "\"%s\" is both placeable and a texture - the drop cannot choose", c.path);

    // ---- and what a .daimat CARRIES --------------------------------------
    //
    // The same question one layer down: the browser knows a material file when
    // it sees one, and this is what it finds inside. Two properties, and the
    // first one is the one that breaks projects: a file written before the
    // maps and the world projection existed still opens, unchanged, and saving
    // it again does not sprinkle the new keys through it.
    std::printf("\nwhat a .daimat carries\n");
    {
        dai_matfile d = dai_matfile_default();
        CHECK(d.triplanar == 0, "a material projects in world space by default - it must not");
        CHECK(d.triplanar_scale == 1.0f && d.triplanar_blend == 4.0f,
              "the projection defaults moved: %g m, blend %g",
              (double)d.triplanar_scale, (double)d.triplanar_blend);
        CHECK(d.normal_strength == 1.0f && d.base_color_map[0] == 0 && d.orm_map[0] == 0 &&
              d.normal_map[0] == 0, "a fresh material is not empty in the new fields");

        const char *old_text = "daidalos-material 1\ncolor 0.82 0.24 0.2\nroughness 0.35\n";
        dai_matfile old_m{};
        CHECK(dai_matfile_from_text(&old_m, old_text, std::strlen(old_text)) == DAI_OK,
              "a material file written before this round no longer loads");
        CHECK(old_m.color.x > 0.81f && old_m.color.x < 0.83f && old_m.roughness == 0.35f,
              "the old fields changed meaning: color.x %g roughness %g",
              (double)old_m.color.x, (double)old_m.roughness);
        CHECK(old_m.triplanar == 0 && old_m.triplanar_scale == 1.0f && old_m.base_color_map[0] == 0,
              "a file with no projection line did not come back as an unprojected material");
        char short_buf[2048];
        dai_matfile_to_text(&old_m, short_buf, sizeof(short_buf));
        CHECK(std::strstr(short_buf, "triplanar") == nullptr &&
              std::strstr(short_buf, "_map") == nullptr,
              "saving an untouched old material wrote the new keys into it:\n%s", short_buf);

        // A new one, with everything the world projection needs.
        dai_matfile m = dai_matfile_default();
        m.color = dai_vec3{ 0.7f, 0.68f, 0.66f };
        std::snprintf(m.base_color_map, sizeof(m.base_color_map), "textures/raufaser_wand_basecolor.png");
        std::snprintf(m.orm_map, sizeof(m.orm_map), "textures/raufaser_wand_orm.png");
        std::snprintf(m.normal_map, sizeof(m.normal_map), "textures/raufaser_wand_normal.png");
        m.normal_strength = 0.75f;
        m.triplanar = 1;
        m.triplanar_scale = 2.0f;
        m.triplanar_blend = 6.0f;
        char buf[2048];
        size_t n = dai_matfile_to_text(&m, buf, sizeof(buf));
        CHECK(n > 0 && n < sizeof(buf), "the material text did not fit (%u bytes)", (unsigned)n);
        dai_matfile back{};
        CHECK(dai_matfile_from_text(&back, buf, std::strlen(buf)) == DAI_OK,
              "the material this round writes does not load again:\n%s", buf);
        CHECK(std::strcmp(back.base_color_map, m.base_color_map) == 0 &&
              std::strcmp(back.orm_map, m.orm_map) == 0 &&
              std::strcmp(back.normal_map, m.normal_map) == 0,
              "a map path did not survive the round trip: '%s' '%s' '%s'",
              back.base_color_map, back.orm_map, back.normal_map);
        CHECK(back.triplanar == 1 && back.triplanar_scale == 2.0f && back.triplanar_blend == 6.0f,
              "the projection did not survive the round trip: %d, %g m, blend %g",
              back.triplanar, (double)back.triplanar_scale, (double)back.triplanar_blend);
        CHECK(back.normal_strength == 0.75f, "normal_strength came back as %g",
              (double)back.normal_strength);

        // A path with a space in it is one path, not two tokens.
        const char *spaced = "daidalos-material 1\nbase_color_map textures/old wall.png\n";
        dai_matfile sp{};
        dai_matfile_from_text(&sp, spaced, std::strlen(spaced));
        CHECK(std::strcmp(sp.base_color_map, "textures/old wall.png") == 0,
              "a map path with a space came back as '%s'", sp.base_color_map);

        // Nonsense is clamped on the way in, not three functions later.
        const char *bad = "daidalos-material 1\ntriplanar_scale 0\ntriplanar_blend 900\n";
        dai_matfile cl{};
        dai_matfile_from_text(&cl, bad, std::strlen(bad));
        CHECK(cl.triplanar_scale == 1.0f, "a tiling of 0 m was let through as %g",
              (double)cl.triplanar_scale);
        CHECK(cl.triplanar_blend == 16.0f, "a blend of 900 was let through as %g",
              (double)cl.triplanar_blend);

        // "triplanar" with no number means ON: the shortest thing an author
        // can type has to mean the obvious thing.
        const char *bare = "daidalos-material 1\ntriplanar\n";
        dai_matfile br{};
        dai_matfile_from_text(&br, bare, std::strlen(bare));
        CHECK(br.triplanar == 1, "a bare `triplanar` line did not switch it on");

        // A new material file offers every field, including the ones this
        // round added - a key an author cannot see is a key nobody uses.
        char full[2048];
        dai_matfile_to_text_full(&d, full, sizeof(full));
        CHECK(std::strstr(full, "triplanar_scale") && std::strstr(full, "base_color_map") &&
              std::strstr(full, "normal_strength"),
              "a freshly created material does not offer the map and projection keys:\n%s", full);

        // And the four materials this project ships are readable, projected,
        // and point at maps the baker writes.
        static const char *SHIPPED[] = {
            "projects/Untitled/assets/materials/pvc.daimat",
            "projects/Untitled/assets/materials/raufaser.daimat",
            "projects/Untitled/assets/materials/rost.daimat",
            "projects/Untitled/assets/materials/beton.daimat",
        };
        for (const char *path : SHIPPED) {
            dai_matfile sm{};
            char merr[256] = { 0 };
            if (dai_matfile_load(&sm, path, merr, sizeof(merr)) != DAI_OK) {
                // Run from another directory: not a failure of the format.
                std::printf("  (skipped %s: %s)\n", path, merr);
                continue;
            }
            CHECK(sm.triplanar == 1, "%s is not projected - a blockout wall has no UV set", path);
            CHECK(sm.triplanar_scale >= 0.5f && sm.triplanar_scale <= 8.0f,
                  "%s tiles every %g m, which is not a room-sized number",
                  path, (double)sm.triplanar_scale);
            CHECK(sm.base_color_map[0] && sm.orm_map[0] && sm.normal_map[0],
                  "%s leaves a map slot empty", path);
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
