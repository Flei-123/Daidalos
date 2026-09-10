// scene.spawn() / scene.destroy(): the only way a behaviour may bring
// something into the world, and take it away again.
//
//   ./build/test_spawn
//
// This one is NOT written against a fake host. The fakes in
// tests/test_objmodel.cpp check that the PRELUDE asks for the right names;
// what has to be true here is what happens to a real document: that a copied
// room is really a second room, that its children hang under the copy and not
// under the original, that a subtree is gone when it is destroyed, and that
// the two ways to lose the document - copying a node into itself, spawning
// without end - are refused. So it drives the same include/dai_spawn_host.inl
// every host includes, over a real dai_doc, through real JavaScript.

#include "dai_doc.h"
#include "dai_script.h"
#include "dai_prelude.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static dai_doc *g_doc = nullptr;

// The same two seams the editor, the modelling host and the shipped game
// include. Testing a copy of them would prove nothing about the game.
#define DAI_PROPS_DOC g_doc
#include "dai_props_host.inl"
#include "dai_spawn_host.inl"

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
    if (dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return 0;
    xyz[0] = r.position.x; xyz[1] = r.position.y; xyz[2] = r.position.z;
    return 1;
}
static void sh_set_pos(double id, const double *xyz, void *) {
    dai_node_desc r{};
    if (dai_doc_get(g_doc, (dai_node)(uint32_t)id, &r) != DAI_OK) return;
    r.position = dai_vec3{ (float)xyz[0], (float)xyz[1], (float)xyz[2] };
    dai_doc_set(g_doc, (dai_node)(uint32_t)id, &r);
}
static int  sh_get_rot(double, double *q, void *) { q[0]=q[1]=q[2]=0; q[3]=1; return 1; }
static void sh_set_rot(double, const double *, void *) {}
static void sh_set_text(double, const char *, void *) {}
static double sh_get_num(double id, const char *p, void *) { return comp_get_num((dai_node)(uint32_t)id, p, 0.0); }
static void   sh_set_num(double id, const char *p, double v, void *) { comp_set_num((dai_node)(uint32_t)id, p, v); }
static int    sh_get_vec(double id, const char *p, double *xyz, void *) { return comp_get_vec((dai_node)(uint32_t)id, p, xyz); }
static void   sh_set_vec(double id, const char *p, const double *xyz, void *) { comp_set_vec((dai_node)(uint32_t)id, p, xyz); }
static const char *sh_get_str(double id, const char *p, void *) { return comp_get_str((dai_node)(uint32_t)id, p); }
static void   sh_set_str(double id, const char *p, const char *v, void *) { comp_set_str((dai_node)(uint32_t)id, p, v); }
static double sh_spawn(double s, double p, const char *n, void *) { return comp_spawn((dai_node)(uint32_t)s, (dai_node)(uint32_t)p, n); }
static int    sh_destroy(double id, void *) { return comp_destroy((dai_node)(uint32_t)id); }
static double sh_child_count(double id, void *) { return comp_child_count((dai_node)(uint32_t)id); }
static double sh_child_at(double id, double i, void *) { return comp_child_at((dai_node)(uint32_t)id, i); }
static double sh_parent_of(double id, void *) { return comp_parent_of((dai_node)(uint32_t)id); }

static dai_script_node_host g_host = { sh_find, sh_get_pos, sh_set_pos, sh_get_rot, sh_set_rot,
                                       sh_set_text, sh_get_num, sh_set_num, sh_get_vec, sh_set_vec,
                                       sh_get_str, sh_set_str,
                                       sh_spawn, sh_destroy, sh_child_count, sh_child_at,
                                       sh_parent_of, nullptr };

// ---- a little house ------------------------------------------------------
// A room group with a floor and two walls under it, one of the walls carrying
// a material and a script with parameters - the things that have to survive a
// copy for the copy to be worth anything.
static dai_node add(const char *name, dai_node parent, dai_vec3 pos) {
    dai_node_desc r = dai_node_desc_default();
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    r.parent = parent;
    r.position = pos;
    r.no_body = 1;
    return dai_doc_add(g_doc, &r);
}

static std::string js_str(dai_script *s, const char *expr) {
    std::string src = std::string("state.__r = \"\" + (") + expr + ");";
    char err[512] = {0};
    if (dai_script_eval(s, src.c_str(), "test", err, sizeof(err)) != DAI_OK) {
        std::printf("  JS ERROR in [%s]: %s\n", expr, err);
        return "<error>";
    }
    char out[512] = {0};
    dai_script_get_string(s, "__r", out, sizeof(out));
    return out;
}
static double js_num(dai_script *s, const char *expr) {
    return std::atof(js_str(s, expr).c_str());
}

int main(void) {
    std::printf("test_spawn\n");
    g_doc = dai_doc_create();

    dai_node room  = add("Raum00", 0, dai_vec3{ 0, 0, 0 });
    dai_node floor = add("Raum00.Floor", room, dai_vec3{ 0, -0.1f, 0 });
    dai_node wallA = add("Raum00.Wall.A", room, dai_vec3{ -2, 1.2f, 0 });
    dai_node wallB = add("Raum00.Wall.B", room, dai_vec3{ 2, 1.2f, 0 });
    dai_node lamp  = add("Raum00.Licht.1", 0, dai_vec3{ 0, 2.2f, 0 });   // NOT a child
    (void)floor; (void)lamp;
    {   // the wall carries a material and a behaviour with tuned parameters
        dai_node_desc r{};
        dai_doc_get(g_doc, wallA, &r);
        std::snprintf(r.materials, sizeof(r.materials), "materials/raufaser.daimat");
        std::snprintf(r.script, sizeof(r.script), "innen_door.js{openAngle=88,speed=140}");
        r.light = 1; r.light_intensity = 3.5f;
        dai_doc_set(g_doc, wallA, &r);
    }

    char err[512] = {0};
    dai_script *s = dai_script_create(err, sizeof(err));
    if (!s) { std::printf("  FAIL script host: %s\n", err); return 1; }
    dai_script_bind_nodes(s, &g_host);
    if (dai_script_eval(s, DAI_JS_PRELUDE, "prelude", err, sizeof(err)) != DAI_OK) {
        std::printf("  FAIL prelude: %s\n", err);
        return 1;
    }

    uint32_t before = dai_doc_count(g_doc);

    // ---- 1. a copy is a second room, not a second name for the first -----
    double copy = js_num(s, "scene.spawn(scene.find(\"Raum00\"), 0, \"Raum01\").valueOf()");
    CHECK(copy > 0, "spawn returned %g", copy);
    CHECK((dai_node)copy != room, "the copy is the source (%g)", copy);
    CHECK(dai_doc_count(g_doc) == before + 4,
          "document grew by %u, expected 4 (group + floor + two walls)",
          dai_doc_count(g_doc) - before);
    {
        dai_node_desc r{};
        dai_doc_get(g_doc, (dai_node)copy, &r);
        CHECK(std::strcmp(r.name, "Raum01") == 0, "copy is called '%s'", r.name);
        CHECK(r.parent == 0, "copy hangs under %u, expected the root", (unsigned)r.parent);
    }

    // ---- 2. the subtree came with it, under the COPY ----------------------
    CHECK(js_num(s, "scene.childCount(scene.find(\"Raum01\"))") == 3,
          "the copy has %g children, expected 3", js_num(s, "scene.childCount(scene.find(\"Raum01\"))"));
    CHECK(js_num(s, "scene.childCount(scene.find(\"Raum00\"))") == 3,
          "the source lost children");
    {   // every child of the copy points at the copy, and none of the source's
        // children moved - the mistake a flat copy makes
        uint32_t n = dai_doc_count(g_doc);
        std::vector<dai_node> all(n);
        dai_doc_nodes(g_doc, all.data(), n);
        int under_copy = 0, under_src = 0;
        for (uint32_t i = 0; i < n; ++i) {
            dai_node_desc r{};
            if (dai_doc_get(g_doc, all[i], &r) != DAI_OK) continue;
            if (r.parent == (dai_node)copy) under_copy++;
            if (r.parent == room) under_src++;
        }
        CHECK(under_copy == 3, "%d nodes hang under the copy, expected 3", under_copy);
        CHECK(under_src == 3, "%d nodes hang under the source, expected 3", under_src);
    }

    // ---- 3. what makes a copy worth having: it is the same record ---------
    // The copied wall keeps its material, its behaviour AND the parameters
    // that were tuned into it, and its light. A copy that loses those is a
    // grey box where a room was.
    dai_node copy_wall = 0;
    {
        uint32_t c = dai_doc_children(g_doc, (dai_node)copy, nullptr, 0);
        std::vector<dai_node> kids(c);
        dai_doc_children(g_doc, (dai_node)copy, kids.data(), c);
        for (uint32_t i = 0; i < c; ++i) {
            dai_node_desc r{};
            dai_doc_get(g_doc, kids[i], &r);
            if (std::strcmp(r.name, "Raum00.Wall.A") == 0) copy_wall = kids[i];
        }
    }
    CHECK(copy_wall != 0, "the copied wall was not found by name");
    if (copy_wall) {
        dai_node_desc r{};
        dai_doc_get(g_doc, copy_wall, &r);
        CHECK(std::strcmp(r.materials, "materials/raufaser.daimat") == 0,
              "the copy's material is '%s'", r.materials);
        CHECK(std::strstr(r.script, "innen_door.js") && std::strstr(r.script, "speed=140"),
              "the copy's script is '%s'", r.script);
        CHECK(r.light == 1 && r.light_intensity > 3.4f && r.light_intensity < 3.6f,
              "the copy's light is %d at %.2f", r.light, (double)r.light_intensity);
        CHECK(r.position.x < -1.9f && r.position.x > -2.1f,
              "the copy's wall sits at x=%.2f, expected the source's -2", (double)r.position.x);
    }

    // ---- 4. the copy is independent -------------------------------------
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "node.setPos(%u, 9, 0, 9)", (unsigned)copy);
        dai_script_eval(s, buf, "test", err, sizeof(err));
        dai_node_desc a{}, b{};
        dai_doc_get(g_doc, (dai_node)copy, &a);
        dai_doc_get(g_doc, room, &b);
        CHECK(a.position.x > 8.9f, "the copy did not move (%.2f)", (double)a.position.x);
        CHECK(b.position.x < 0.1f && b.position.x > -0.1f,
              "moving the copy moved the source (%.2f)", (double)b.position.x);
    }

    // ---- 5. material by name, the property the rebuild rule re-skins with --
    {
        CHECK(js_str(s, "node.getStr(scene.find(\"Raum00\").valueOf(), \"material\")") == "",
              "an empty material stack answered something");
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "node.setStr(%u, \"material\", \"materials/beton.daimat\")",
                      (unsigned)copy_wall);
        dai_script_eval(s, buf, "test", err, sizeof(err));
        dai_node_desc r{};
        dai_doc_get(g_doc, copy_wall, &r);
        CHECK(std::strcmp(r.materials, "materials/beton.daimat") == 0,
              "material.slot0 is '%s'", r.materials);
        dai_node_desc src_r{};
        dai_doc_get(g_doc, wallA, &src_r);
        CHECK(std::strcmp(src_r.materials, "materials/raufaser.daimat") == 0,
              "re-skinning the copy re-skinned the source");
    }

    // ---- 6. destroy takes the subtree, and nothing else -------------------
    {
        uint32_t n0 = dai_doc_count(g_doc);
        CHECK(js_num(s, "scene.destroy(scene.find(\"Raum01\")) ? 1 : 0") == 1, "destroy refused");
        CHECK(dai_doc_count(g_doc) == n0 - 4,
              "destroy removed %u nodes, expected 4", n0 - dai_doc_count(g_doc));
        CHECK(sh_find("Raum01", nullptr) < 0, "the destroyed room is still findable");
        CHECK(dai_doc_valid(g_doc, room), "destroying the copy killed the source");
        CHECK(dai_doc_valid(g_doc, lamp), "destroying the room killed a node outside it");
    }

    // ---- 7. the two refusals ---------------------------------------------
    // A copy parented into its own source would be its own child. The document
    // rejects the cycle, but by then half the subtree stands - so it is
    // refused before anything is made.
    {
        uint32_t n0 = dai_doc_count(g_doc);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "scene.spawn(%u, %u, \"Loop\").valueOf()",
                      (unsigned)room, (unsigned)wallB);
        CHECK(js_num(s, buf) < 0, "spawning a room into its own wall was allowed");
        CHECK(dai_doc_count(g_doc) == n0, "the refused spawn still made %u nodes",
              dai_doc_count(g_doc) - n0);
        CHECK(js_num(s, "scene.spawn(-1, 0, \"Nothing\").valueOf()") < 0, "spawning a non-node was allowed");
        CHECK(js_num(s, "scene.destroy(-1) ? 1 : 0") == 0, "destroying a non-node was allowed");
        CHECK(js_num(s, "scene.destroy(0) ? 1 : 0") == 0, "destroying the root was allowed");
    }

    // ---- 8. the budget: a spawn loop may not eat the document -------------
    // Doubling a subtree in a loop is the runaway this has a limit for. It is
    // not "it went slow": past DAI_SPAWN_MAX_NODES the spawn is refused and
    // says so, and the document is still a document afterwards.
    {
        double last = 0;
        int refusals = 0;
        for (int i = 0; i < 400; ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "scene.spawn(%u, 0, \"Copy\").valueOf()", (unsigned)room);
            last = js_num(s, buf);
            if (last < 0) { refusals++; break; }
            // ...and hang the copy back under the source, so the source's
            // subtree DOUBLES every round. Ten rounds is the whole budget.
            dai_doc_set_parent(g_doc, (dai_node)last, room);
        }
        CHECK(refusals == 1, "the growing subtree was never refused (%d)", refusals);
        CHECK(dai_doc_count(g_doc) < DAI_SPAWN_MAX_NODES * 4,
              "the document ran to %u nodes", dai_doc_count(g_doc));
        CHECK(dai_doc_valid(g_doc, room), "the source is gone after the runaway");
    }

    // ---- 9. the walk: parent/childAt agree with each other ----------------
    {
        dai_node fresh_parent = add("Halle", 0, dai_vec3{ 0, 0, 0 });
        dai_node kid = add("Halle.Kind", fresh_parent, dai_vec3{ 1, 0, 0 });
        char buf[256];
        std::snprintf(buf, sizeof(buf), "scene.childAt(%u, 0).valueOf()", (unsigned)fresh_parent);
        CHECK((dai_node)js_num(s, buf) == kid, "childAt(0) is not the child");
        std::snprintf(buf, sizeof(buf), "scene.parent(%u).valueOf()", (unsigned)kid);
        CHECK((dai_node)js_num(s, buf) == fresh_parent, "parent() is not the parent");
        std::snprintf(buf, sizeof(buf), "scene.childAt(%u, 7).valueOf()", (unsigned)fresh_parent);
        CHECK(js_num(s, buf) < 0, "childAt past the end answered a node");
        std::snprintf(buf, sizeof(buf), "scene.children(%u).length", (unsigned)fresh_parent);
        CHECK(js_num(s, buf) == 1, "children() gave the wrong length");
    }

    dai_script_destroy(s);
    dai_doc_destroy(g_doc);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
