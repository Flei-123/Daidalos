// The game's HUD: where a label actually lands.
//
//   ./build/test_hud
//
// No GPU and no window. dai_hud_draw emits quads through dai_ui, and a quad
// has coordinates - so "is the score in the top left corner" is a question
// with a numeric answer, and every anchor can be checked in a millisecond
// instead of by squinting at a screenshot.
//
// The case that made this file worth writing: a HUD node has to be `hidden`,
// because every node in this engine draws a box unless it is, and a label
// with a grey box behind it is not a label. dai_hud_draw honoured `hidden`
// and skipped it - so the only two states available were "box in the middle
// of the game" and "nothing at all".

#include "dai_editor_ui.h"
#include "dai_ui.h"
#include "dai_font.h"
#include "dai_doc.h"
#include "dai_strings.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

struct Box { float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; int quads = 0; };

// The bounding box of everything drawn, read straight out of the draw list.
static Box measure(dai_ui *ui) {
    Box b;
    const dai_ui_draw *draws = nullptr;
    uint32_t nb = dai_ui_draws(ui, &draws);
    for (uint32_t i = 0; i < nb; ++i) {
        for (uint32_t v = 0; v < draws[i].count; ++v) {
            const dai_ui_vertex &p = draws[i].vertices[v];
            if (p.x < b.x0) b.x0 = p.x;
            if (p.y < b.y0) b.y0 = p.y;
            if (p.x > b.x1) b.x1 = p.x;
            if (p.y > b.y1) b.y1 = p.y;
        }
        b.quads += (int)draws[i].count / 6;
    }
    return b;
}

static dai_strings *g_tab = nullptr;
static const char *resolve(const char *text, void *) {
    static char buf[256];
    return dai_strings_resolve(g_tab, text, buf, sizeof(buf));
}

static Box run(dai_ui *ui, dai_doc *doc, float w, float h) {
    dai_ui_input in{};
    dai_ui_begin(ui, w, h, &in);
    dai_hud_draw(ui, doc, 0, 0, w, h, 1.0f, resolve, nullptr);
    dai_ui_end(ui);
    return measure(ui);
}

static dai_node add_label(dai_doc *d, const char *name, const char *text, int anchor,
                          float size, int hidden) {
    dai_node_desc r = dai_node_desc_default();
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    r.text_on = 1;
    std::snprintf(r.text, sizeof(r.text), "%s", text);
    r.text_anchor = anchor;
    r.text_size = size;
    r.hidden = hidden;
    r.no_body = 1;
    return dai_doc_add(d, &r);
}

int main() {
    std::printf("game HUD\n");
    char err[256] = { 0 };
    dai_font *font = dai_font_load_ui(13.0f, err, sizeof(err));
    if (!font) { std::printf("no system font (%s) - skipping\n", err); std::printf("\n0 passed, 0 failed\n"); return 0; }
    dai_ui *ui = dai_ui_create(font, 1);
    CHECK(ui != nullptr, "ui creation failed");

    g_tab = dai_strings_create();
    const char *DE = "daidalos-strings 1\nlang de\nhud.score Punkte\n"
                     "hud.hint erste Zeile\\nzweite Zeile\n";
    CHECK(dai_strings_parse(g_tab, DE, std::strlen(DE), err, sizeof(err)) == DAI_OK,
          "the table did not parse: %s", err);

    const float W = 1280.0f, H = 720.0f;

    // ---- 1. a hidden node still shows its label ---------------------------
    {
        dai_doc *d = dai_doc_create();
        add_label(d, "Score", "Score", 0, 24.0f, /*hidden*/1);
        Box b = run(ui, d, W, H);
        CHECK(b.quads > 0, "a hidden node drew NO label - 'hidden' is the mesh renderer's "
                           "checkbox, and a label-only object is hidden by construction");
        CHECK(b.x1 < W * 0.5f && b.y1 < H * 0.5f,
              "anchor 0 should be top left, the label is at (%.0f..%.0f, %.0f..%.0f)",
              b.x0, b.x1, b.y0, b.y1);
        dai_doc_destroy(d);
    }

    // ---- 2. `disabled` DOES turn it off -----------------------------------
    {
        dai_doc *d = dai_doc_create();
        dai_node n = add_label(d, "Score", "Score", 0, 24.0f, 0);
        dai_node_desc r{};
        dai_doc_get(d, n, &r);
        r.disabled = 1;
        dai_doc_set(d, n, &r);
        Box b = run(ui, d, W, H);
        CHECK(b.quads == 0, "a disabled object still drew its label (%d quads)", b.quads);
        dai_doc_destroy(d);
    }

    // ---- 3. every anchor lands in its own corner --------------------------
    struct Case { int anchor; const char *name; int right; int bottom; int centre_x; int centre_y; };
    static const Case CASES[] = {
        { 0, "top left",     0, 0, 0, 0 },
        { 2, "top right",    1, 0, 0, 0 },
        { 4, "centre",       0, 0, 1, 1 },
        { 6, "bottom left",  0, 1, 0, 0 },
        { 8, "bottom right", 1, 1, 0, 0 },
    };
    for (const Case &c : CASES) {
        dai_doc *d = dai_doc_create();
        add_label(d, "L", "Score", c.anchor, 24.0f, 1);
        Box b = run(ui, d, W, H);
        CHECK(b.quads > 0, "%s drew nothing", c.name);
        float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
        if (c.centre_x) CHECK(std::fabs(cx - W * 0.5f) < W * 0.1f,
                              "%s: x centre is %.0f, expected near %.0f", c.name, cx, W * 0.5f);
        else if (c.right) CHECK(cx > W * 0.6f, "%s: x centre is %.0f, expected the right", c.name, cx);
        else CHECK(cx < W * 0.4f, "%s: x centre is %.0f, expected the left", c.name, cx);
        if (c.centre_y) CHECK(std::fabs(cy - H * 0.5f) < H * 0.15f,
                              "%s: y centre is %.0f, expected near %.0f", c.name, cy, H * 0.5f);
        else if (c.bottom) CHECK(cy > H * 0.6f, "%s: y centre is %.0f, expected the bottom", c.name, cy);
        else CHECK(cy < H * 0.4f, "%s: y centre is %.0f, expected the top", c.name, cy);
        // Never outside the view. A HUD that leaves its rectangle draws over
        // whatever the Game view is docked next to.
        CHECK(b.x0 >= -1.0f && b.y0 >= -1.0f && b.x1 <= W + 1.0f && b.y1 <= H + 1.0f,
              "%s left the view: (%.0f..%.0f, %.0f..%.0f)", c.name, b.x0, b.x1, b.y0, b.y1);
        dai_doc_destroy(d);
    }

    // ---- 4. a key resolves, and a missing one shows the key ---------------
    {
        dai_doc *d = dai_doc_create();
        add_label(d, "S", "@hud.score", 0, 24.0f, 1);
        Box withkey = run(ui, d, W, H);
        dai_doc_destroy(d);

        dai_doc *e = dai_doc_create();
        add_label(e, "S", "Punkte", 0, 24.0f, 1);
        Box plain = run(ui, e, W, H);
        dai_doc_destroy(e);
        CHECK(withkey.quads > 0, "the @key label drew nothing");
        CHECK(withkey.quads == plain.quads,
              "'@hud.score' drew %d quads, 'Punkte' drew %d - the lookup did not happen",
              withkey.quads, plain.quads);

        dai_doc *f = dai_doc_create();
        add_label(f, "S", "@nope.nope", 0, 24.0f, 1);
        Box missing = run(ui, f, W, H);
        dai_doc_destroy(f);
        CHECK(missing.quads > 0, "a missing key drew NOTHING - it has to show the key, "
                                 "or a half translated build looks broken instead of unfinished");
    }

    // ---- 5. two lines are twice as tall -----------------------------------
    {
        dai_doc *d = dai_doc_create();
        add_label(d, "H", "@hud.hint", 6, 20.0f, 1);   // bottom left
        Box two = run(ui, d, W, H);
        dai_doc_destroy(d);

        dai_doc *e = dai_doc_create();
        add_label(e, "H", "erste Zeile", 6, 20.0f, 1);
        Box one = run(ui, e, W, H);
        dai_doc_destroy(e);

        CHECK(two.quads > one.quads, "the two line label drew %d quads, one line drew %d",
              two.quads, one.quads);
        CHECK(two.y1 - two.y0 > (one.y1 - one.y0) * 1.5f,
              "the two line label is %.0f px tall, one line is %.0f",
              two.y1 - two.y0, one.y1 - one.y0);
        CHECK(two.y1 <= H + 1.0f, "the second line fell out of the view at y %.0f", two.y1);
    }

    // ---- 6. size actually changes the size --------------------------------
    {
        dai_doc *d = dai_doc_create();
        add_label(d, "S", "Score", 0, 16.0f, 1);
        Box small = run(ui, d, W, H);
        dai_doc_destroy(d);
        dai_doc *e = dai_doc_create();
        add_label(e, "S", "Score", 0, 48.0f, 1);
        Box big = run(ui, e, W, H);
        dai_doc_destroy(e);
        float rs = (small.x1 - small.x0), rb = (big.x1 - big.x0);
        CHECK(rb > rs * 2.0f, "48 px is %.0f wide, 16 px is %.0f - size did nothing", rb, rs);
    }

    // ---- 7. the box: wrapping and autosize ---------------------------------
    // The case this exists for: a translated string is never the length the
    // layout was drawn for. German runs about a third longer than English,
    // and the box does not get bigger when it does.
    {
        const char *LONG = "Press the space bar to jump and hold shift to sprint across the floor";
        dai_doc *d = dai_doc_create();
        dai_node n = add_label(d, "Hint", LONG, 0, 20.0f, 1);
        Box nobox = run(ui, d, W, H);

        // With a width, it WRAPS: the same words, narrower, taller.
        dai_node_desc r{};
        dai_doc_get(d, n, &r);
        r.text_w = 240.0f;
        dai_doc_set(d, n, &r);
        Box wrapped = run(ui, d, W, H);
        CHECK(wrapped.x1 - wrapped.x0 <= 244.0f,
              "the wrapped label is %.0f px wide, the box is 240", wrapped.x1 - wrapped.x0);
        CHECK(wrapped.y1 - wrapped.y0 > (nobox.y1 - nobox.y0) * 1.5f,
              "wrapping did not make it taller: %.0f px vs %.0f",
              wrapped.y1 - wrapped.y0, nobox.y1 - nobox.y0);
        CHECK(wrapped.quads == nobox.quads,
              "wrapping changed the number of glyphs (%d vs %d) - it dropped or "
              "duplicated words", wrapped.quads, nobox.quads);

        // With a HEIGHT and autosize, it shrinks until it fits that height.
        r.text_h = 44.0f;
        r.text_autosize = 1;
        dai_doc_set(d, n, &r);
        Box fitted = run(ui, d, W, H);
        CHECK(fitted.y1 - fitted.y0 <= 48.0f,
              "autosize left the text %.0f px tall in a 44 px box", fitted.y1 - fitted.y0);
        CHECK(fitted.quads == nobox.quads, "autosize lost characters (%d vs %d)",
              fitted.quads, nobox.quads);
        CHECK(fitted.x1 - fitted.x0 <= 244.0f, "autosize left it %.0f px wide",
              fitted.x1 - fitted.x0);
        dai_doc_destroy(d);
    }

    // ---- what counts as a texture -----------------------------------------
    // One answer, two callers: the picker that LISTS textures and the drop
    // that ACCEPTS one. When those disagreed, a file was offered in the list
    // and then refused by the drag - which reads as "drag and drop is broken"
    // rather than "two extension tests differ".
    std::printf("texture files\n");
    {
        const char *yes[] = {
            "Textures/wall.png", "logo.PNG", "a.jpg", "a.JPEG", "sprites/hero.tga",
            "deep/folder.with.dots/thing.png",
        };
        for (const char *f : yes)
            CHECK(dai_editor_ui_is_texture(f) == 1, "'%s' should be a texture", f);
        const char *no[] = {
            "Scenes/level.daidalos", "mat.daimat", "Scripts/player.js", "readme.md",
            "png", ".png", "Textures/.png", "no_extension", "model.glb",
            "sound.wav", "", "picture.png.txt",
        };
        for (const char *f : no)
            CHECK(dai_editor_ui_is_texture(f) == 0, "'%s' should NOT be a texture", f);
        CHECK(dai_editor_ui_is_texture(nullptr) == 0, "a null path crashed or said yes");
    }

    dai_strings_destroy(g_tab);
    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
