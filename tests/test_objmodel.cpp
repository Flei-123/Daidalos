// The object model behaviours are written against: self.transform.position.x
//
//   ./build/test_objmodel
//
// The prelude is JavaScript that the editor evaluates before every behaviour.
// It has to do two things and it is easy to get one of them right and lose the
// other:
//
//   1. self.transform.position.x reads AND writes through to the engine
//   2. self is still a NUMBER wherever a number is expected, so every script
//      written before the model existed - body.setVel(self, ...) - keeps
//      working. That is what valueOf() is for, and it is the whole reason the
//      change is safe to make at all.
//
// This test drives the same bindings the editor installs, against a fake node
// host, so "it wrote through" is a value in a C++ array and not an impression.

#include "dai_script.h"
#include "dai_prelude.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

// ---- the fake world ------------------------------------------------------
static double g_pos[4][3] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0}};
static double g_rot[4][4] = {{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1}};
static double g_vel[4][3] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0}};
static std::string g_text[4];
static int  g_impulses = 0;

static int idx(double id) { int i = (int)id; return (i >= 0 && i < 4) ? i : 0; }

static double fh_find(const char *name, void *) { return std::strcmp(name, "Camera") == 0 ? 2.0 : -1.0; }
static int  fh_get_pos(double id, double *xyz, void *) {
    for (int i = 0; i < 3; ++i) xyz[i] = g_pos[idx(id)][i];
    return 1;
}
static void fh_set_pos(double id, const double *xyz, void *) {
    for (int i = 0; i < 3; ++i) g_pos[idx(id)][i] = xyz[i];
}
static int  fh_get_rot(double id, double *q, void *) {
    for (int i = 0; i < 4; ++i) q[i] = g_rot[idx(id)][i];
    return 1;
}
static void fh_set_rot(double id, const double *q, void *) {
    for (int i = 0; i < 4; ++i) g_rot[idx(id)][i] = q[i];
}
static void fh_set_text(double id, const char *t, void *) { g_text[idx(id)] = t ? t : ""; }


// ---- the fake components -------------------------------------------------
// The property bridge is "a name goes in, a field comes out", so the fake is
// a map. It checks the CONTRACT - that the prelude asks for the right names
// and writes through - not what the editor does with them, which is
// tests/test_editor's job.
#include <map>
static std::map<std::string, double> g_num;
static std::map<std::string, double[3]> g_vecdummy;   // unused, kept out of the way
static std::map<std::string, std::string> g_str;
static double g_vec[64][3];
static std::map<std::string, int> g_vecslot;
static int g_vecn = 0;

static std::string key(double id, const char *p) {
    char b[128]; std::snprintf(b, sizeof(b), "%d/%s", (int)id, p ? p : "");
    return b;
}
static double fh_get_num(double id, const char *p, void *) {
    auto it = g_num.find(key(id, p));
    return it == g_num.end() ? 0.0 : it->second;
}
static void fh_set_num(double id, const char *p, double v, void *) { g_num[key(id, p)] = v; }
static int fh_get_vec(double id, const char *p, double *xyz, void *) {
    auto it = g_vecslot.find(key(id, p));
    if (it == g_vecslot.end()) { xyz[0] = xyz[1] = xyz[2] = 0; return 0; }
    for (int i = 0; i < 3; ++i) xyz[i] = g_vec[it->second][i];
    return 1;
}
static void fh_set_vec(double id, const char *p, const double *xyz, void *) {
    std::string k = key(id, p);
    if (!g_vecslot.count(k)) g_vecslot[k] = g_vecn++;
    for (int i = 0; i < 3; ++i) g_vec[g_vecslot[k]][i] = xyz[i];
}
static const char *fh_get_str(double id, const char *p, void *) {
    static std::string last;
    auto it = g_str.find(key(id, p));
    last = it == g_str.end() ? "" : it->second;
    return last.c_str();
}
static void fh_set_str(double id, const char *p, const char *v, void *) {
    g_str[key(id, p)] = v ? v : "";
}

static int  fp_key(const char *, void *) { return 0; }
static int  fp_get_vel(double id, double *xyz, void *) {
    for (int i = 0; i < 3; ++i) xyz[i] = g_vel[idx(id)][i];
    return 1;
}
static void fp_set_vel(double id, const double *xyz, void *) {
    for (int i = 0; i < 3; ++i) g_vel[idx(id)][i] = xyz[i];
}
static void fp_impulse(double, const double *, void *) { ++g_impulses; }
static int  fp_grounded(double, void *) { return 1; }
static void fp_mouse(double *dx, double *dy, int *b, void *) {
    if (dx) *dx = 0; if (dy) *dy = 0; if (b) *b = 0;
}

int main() {
    std::printf("script object model\n");
    char err[512] = { 0 };
    dai_script *s = dai_script_create(err, sizeof(err));
    CHECK(s != nullptr, "script context: %s", err);
    if (!s) return 1;

    dai_script_node_host nh{};
    nh.find = fh_find; nh.get_pos = fh_get_pos; nh.set_pos = fh_set_pos;
    nh.get_rot = fh_get_rot; nh.set_rot = fh_set_rot; nh.set_text = fh_set_text;
    nh.get_num = fh_get_num; nh.set_num = fh_set_num;
    nh.get_vec = fh_get_vec; nh.set_vec = fh_set_vec;
    nh.get_str = fh_get_str; nh.set_str = fh_set_str;
    dai_script_bind_nodes(s, &nh);

    dai_script_play_host ph{};
    ph.key = fp_key; ph.get_vel = fp_get_vel; ph.set_vel = fp_set_vel;
    ph.impulse = fp_impulse; ph.grounded = fp_grounded; ph.mouse = fp_mouse;
    dai_script_bind_play(s, &ph);

    // The prelude the EDITOR installs, not a copy of it: include/dai_prelude.h
    // is the one and only text, so this test cannot pass while the editor
    // ships something else - which is what the old copy in this file allowed.
    CHECK(dai_script_eval(s, DAI_JS_PRELUDE, "prelude", err, sizeof(err)) == DAI_OK,
          "the prelude did not run: %s", err);
    dai_script_eval(s, "globalThis.state = globalThis.state || {};", "state", err, sizeof(err));
    CHECK(dai_script_eval(s, "var self = __wrapSelf(1);", "self", err, sizeof(err)) == DAI_OK,
          "self did not wrap: %s", err);

    // ---- 1. reading through --------------------------------------------
    g_pos[1][0] = 3.5; g_pos[1][1] = -2.0; g_pos[1][2] = 7.25;
    CHECK(dai_script_eval(s, "state.rx = self.transform.position.x;"
                             "state.ry = self.transform.position.y;"
                             "state.rz = self.position.z;", "read", err, sizeof(err)) == DAI_OK,
          "read failed: %s", err);
    CHECK(std::fabs(dai_script_get_number(s, "rx", -99) - 3.5) < 1e-6, "x read back %.3f",
          dai_script_get_number(s, "rx", -99));
    CHECK(std::fabs(dai_script_get_number(s, "ry", -99) + 2.0) < 1e-6, "y read back %.3f",
          dai_script_get_number(s, "ry", -99));
    CHECK(std::fabs(dai_script_get_number(s, "rz", -99) - 7.25) < 1e-6, "z read back %.3f",
          dai_script_get_number(s, "rz", -99));

    // ---- 2. WRITING through, one component at a time ---------------------
    // The case the whole design turns on: setting .x must not clear .y and .z.
    CHECK(dai_script_eval(s, "self.transform.position.x = 10;", "write", err, sizeof(err)) == DAI_OK,
          "write failed: %s", err);
    CHECK(std::fabs(g_pos[1][0] - 10.0) < 1e-6, "x is %.3f in the world, expected 10", g_pos[1][0]);
    CHECK(std::fabs(g_pos[1][1] + 2.0) < 1e-6,
          "setting x wiped y: it is %.3f, it was -2", g_pos[1][1]);
    CHECK(std::fabs(g_pos[1][2] - 7.25) < 1e-6,
          "setting x wiped z: it is %.3f, it was 7.25", g_pos[1][2]);

    // ---- 3. the whole vector, and the helpers ---------------------------
    dai_script_eval(s, "self.position.set(1, 2, 3);", "set", err, sizeof(err));
    CHECK(g_pos[1][0] == 1 && g_pos[1][1] == 2 && g_pos[1][2] == 3,
          "set() wrote (%.1f %.1f %.1f)", g_pos[1][0], g_pos[1][1], g_pos[1][2]);
    dai_script_eval(s, "self.position.add(0.5, 0, -1);", "add", err, sizeof(err));
    CHECK(std::fabs(g_pos[1][0] - 1.5) < 1e-6 && std::fabs(g_pos[1][2] - 2.0) < 1e-6,
          "add() wrote (%.1f %.1f %.1f)", g_pos[1][0], g_pos[1][1], g_pos[1][2]);

    // ---- 4. THE COMPATIBILITY RULE --------------------------------------
    // Every script written before this existed passes `self` where a number
    // is expected. If that breaks, the model is not worth having.
    g_vel[1][0] = 0;
    CHECK(dai_script_eval(s, "body.setVel(self, 4, 0, 0);", "old", err, sizeof(err)) == DAI_OK,
          "the old spelling threw: %s", err);
    CHECK(std::fabs(g_vel[1][0] - 4.0) < 1e-6,
          "body.setVel(self, ...) wrote velocity %.2f - valueOf() is not doing its job",
          g_vel[1][0]);
    CHECK(dai_script_eval(s, "node.setPos(self, 9, 9, 9);", "old2", err, sizeof(err)) == DAI_OK,
          "node.setPos(self, ...) threw: %s", err);
    CHECK(g_pos[1][0] == 9, "node.setPos(self, ...) did not reach the world");

    // ---- 5. scene.find gives a Node, and it is still a number ------------
    CHECK(dai_script_eval(s, "var cam = scene.find('Camera');"
                             "cam.transform.position.y = 5;"
                             "state.camid = cam + 0;", "find", err, sizeof(err)) == DAI_OK,
          "scene.find chain threw: %s", err);
    CHECK(std::fabs(g_pos[2][1] - 5.0) < 1e-6,
          "the found node's y is %.2f in the world, expected 5", g_pos[2][1]);
    CHECK(std::fabs(dai_script_get_number(s, "camid", -99) - 2.0) < 1e-6,
          "a found Node did not behave as its id in arithmetic");

    // ---- 6. the rest of the sugar ---------------------------------------
    dai_script_eval(s, "self.velocity = [1, 2, 3];", "vel", err, sizeof(err));
    CHECK(g_vel[1][1] == 2, "velocity setter wrote %.1f", g_vel[1][1]);
    dai_script_eval(s, "state.g = self.grounded ? 1 : 0;", "gr", err, sizeof(err));
    CHECK(dai_script_get_number(s, "g", 0) == 1, "grounded did not come through");
    dai_script_eval(s, "self.text = 'Score: ' + 42;", "txt", err, sizeof(err));
    CHECK(g_text[1] == "Score: 42", "the text setter wrote '%s'", g_text[1].c_str());
    dai_script_eval(s, "self.impulse(0, 5, 0);", "imp", err, sizeof(err));
    CHECK(g_impulses == 1, "impulse() did not reach the body");

    // ---- 7. yaw, in degrees ---------------------------------------------
    dai_script_eval(s, "self.transform.yaw = 90; state.y2 = self.transform.yaw;",
                    "yaw", err, sizeof(err));
    CHECK(std::fabs(dai_script_get_number(s, "y2", 0) - 90.0) < 0.5,
          "yaw round tripped to %.2f, expected 90", dai_script_get_number(s, "y2", 0));


    // ---- 8. the components ----------------------------------------------
    // Each one is a CLASS, and each property writes through to the field the
    // inspector shows. The point of the exercise: self.light.intensity is not
    // a variable on a wrapper object, it is the light.
    CHECK(dai_script_eval(s, "self.light.intensity = 2.5; self.light.range = 12;"
                             "self.light.color = [1, 0.5, 0];"
                             "self.camera.fov = 75; self.camera.mode = 1;"
                             "self.text.size = 32; self.text.value = 'hello';"
                             "self.image.asset = 'logo.png'; self.image.size = [2, 1, 1];"
                             "self.rigidbody.friction = 0.8;"
                             "self.transform.scale = [3, 3, 3];",
                          "components", err, sizeof(err)) == DAI_OK,
          "writing the components threw: %s", err);
    CHECK(g_num["1/light.intensity"] == 2.5, "light.intensity is %.2f in the world",
          g_num["1/light.intensity"]);
    CHECK(g_num["1/light.range"] == 12, "light.range is %.2f", g_num["1/light.range"]);
    CHECK(g_num["1/camera.fov"] == 75, "camera.fov is %.2f", g_num["1/camera.fov"]);
    CHECK(g_num["1/text.size"] == 32, "text.size is %.2f", g_num["1/text.size"]);
    CHECK(g_num["1/rigidbody.friction"] == 0.8, "rigidbody.friction is %.2f",
          g_num["1/rigidbody.friction"]);
    CHECK(g_str["1/text.value"] == "hello", "text.value is '%s'", g_str["1/text.value"].c_str());
    CHECK(g_str["1/image.asset"] == "logo.png", "image.asset is '%s'",
          g_str["1/image.asset"].c_str());
    {
        double v[3] = {0,0,0};
        fh_get_vec(1, "light.color", v, nullptr);
        CHECK(std::fabs(v[0] - 1.0) < 1e-6 && std::fabs(v[1] - 0.5) < 1e-6 && v[2] == 0.0,
              "light.color is (%.2f %.2f %.2f), expected (1 0.5 0)", v[0], v[1], v[2]);
        fh_get_vec(1, "transform.scale", v, nullptr);
        CHECK(v[0] == 3 && v[1] == 3 && v[2] == 3,
              "transform.scale is (%.1f %.1f %.1f)", v[0], v[1], v[2]);
    }

    // Reading gives back what was written, through the same names.
    CHECK(dai_script_eval(s, "state.li = self.light.intensity;"
                             "state.cf = self.camera.fov;"
                             "state.tv = self.text.value.length;"
                             "state.lc = self.light.color[1];",
                          "read components", err, sizeof(err)) == DAI_OK,
          "reading the components threw: %s", err);
    CHECK(std::fabs(dai_script_get_number(s, "li", -1) - 2.5) < 1e-6,
          "light.intensity read back %.2f", dai_script_get_number(s, "li", -1));
    CHECK(std::fabs(dai_script_get_number(s, "cf", -1) - 75.0) < 1e-6,
          "camera.fov read back %.2f", dai_script_get_number(s, "cf", -1));
    CHECK(dai_script_get_number(s, "tv", -1) == 5, "text.value read back a string of length %.0f",
          dai_script_get_number(s, "tv", -1));
    CHECK(std::fabs(dai_script_get_number(s, "lc", -1) - 0.5) < 1e-6,
          "light.color[1] read back %.2f", dai_script_get_number(s, "lc", -1));

    // A flag is a boolean on this side of the bridge, whatever it is on the
    // other one.
    dai_script_eval(s, "self.light.enabled = false; state.le = self.light.enabled ? 1 : 0;",
                    "flag", err, sizeof(err));
    CHECK(dai_script_get_number(s, "le", -1) == 0, "light.enabled did not come back false");

    // ---- 9. THE COMPATIBILITY RULE, again --------------------------------
    // `self.text = "..."` was how every HUD script written before components
    // existed set its label. It has to keep meaning exactly that.
    g_text[1].clear();
    dai_script_eval(s, "self.text = 'still works';", "oldtext", err, sizeof(err));
    CHECK(g_text[1] == "still works",
          "self.text = ... stopped reaching node.setText - it wrote '%s'", g_text[1].c_str());
    dai_script_eval(s, "state.ts = '' + self.text;", "textstr", err, sizeof(err));
    CHECK(dai_script_get_number(s, "camid", -99) == 2, "the earlier state was clobbered");

    // Every component is reachable by name from a found node too, not just
    // from self - the same class, the same properties.
    dai_script_eval(s, "scene.find('Camera').camera.fov = 33;", "findcomp", err, sizeof(err));
    CHECK(g_num["2/camera.fov"] == 33, "a found node's camera.fov is %.1f",
          g_num["2/camera.fov"]);

    dai_script_destroy(s);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
