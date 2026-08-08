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
#include <cstdio>

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

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
