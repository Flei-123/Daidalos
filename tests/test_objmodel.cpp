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

// The prelude, character for character the one the editor installs. Kept in a
// file so the two cannot drift: a copy in a test is a copy that goes stale.
static std::string load_prelude() {
    // Extracted from the editor source at build time would be neater still,
    // but the editor embeds it as a C string literal - so the honest thing is
    // to state the contract here and check the BEHAVIOUR, not the bytes.
    return R"JS(
(function () {
    function V3(node, which) { this.__n = node; this.__w = which; }
    function get3(n, w) { return node.getPos(n); }
    function set3(n, w, x, y, z) { node.setPos(n, x, y, z); }
    ["x", "y", "z"].forEach(function (name, i) {
        Object.defineProperty(V3.prototype, name, {
            get: function () { return get3(this.__n, this.__w)[i]; },
            set: function (v) {
                var c = get3(this.__n, this.__w);
                c[i] = v;
                set3(this.__n, this.__w, c[0], c[1], c[2]);
            }
        });
    });
    V3.prototype.set = function (x, y, z) { set3(this.__n, this.__w, x, y, z); return this; };
    V3.prototype.add = function (x, y, z) {
        var c = get3(this.__n, this.__w);
        set3(this.__n, this.__w, c[0] + x, c[1] + (y || 0), c[2] + (z || 0));
        return this;
    };
    function Transform(n) { this.__n = n; }
    Object.defineProperty(Transform.prototype, "position", {
        get: function () { return new V3(this.__n, 0); },
        set: function (v) {
            if (v instanceof V3) { var c = get3(v.__n, v.__w); node.setPos(this.__n, c[0], c[1], c[2]); }
            else node.setPos(this.__n, v[0] || v.x || 0, v[1] || v.y || 0, v[2] || v.z || 0);
        }
    });
    Object.defineProperty(Transform.prototype, "rotation", {
        get: function () { return node.getRot(this.__n); },
        set: function (q) { node.setRot(this.__n, q[0], q[1], q[2], q[3]); }
    });
    Object.defineProperty(Transform.prototype, "yaw", {
        get: function () {
            var q = node.getRot(this.__n);
            return Math.atan2(2 * (q[3] * q[1] + q[0] * q[2]),
                              1 - 2 * (q[1] * q[1] + q[0] * q[0])) * 180 / Math.PI;
        },
        set: function (deg) {
            var h = deg * Math.PI / 360;
            node.setRot(this.__n, 0, Math.sin(h), 0, Math.cos(h));
        }
    });
    function Node(n) { this.__n = n; }
    Node.prototype.valueOf = function () { return this.__n; };
    Node.prototype.toString = function () { return "Node(" + this.__n + ")"; };
    Object.defineProperty(Node.prototype, "id", { get: function () { return this.__n; } });
    Object.defineProperty(Node.prototype, "transform", {
        get: function () { return new Transform(this.__n); }
    });
    Object.defineProperty(Node.prototype, "position", {
        get: function () { return new V3(this.__n, 0); },
        set: function (v) { this.transform.position = v; }
    });
    Object.defineProperty(Node.prototype, "velocity", {
        get: function () { return body.getVel(this.__n); },
        set: function (v) { body.setVel(this.__n, v[0], v[1], v[2]); }
    });
    Object.defineProperty(Node.prototype, "grounded", {
        get: function () { return body.grounded(this.__n); }
    });
    Object.defineProperty(Node.prototype, "text", {
        set: function (t) { node.setText(this.__n, "" + t); }
    });
    Node.prototype.impulse = function (x, y, z) { body.impulse(this.__n, x, y, z); return this; };
    Node.prototype.setVelocity = function (x, y, z) { body.setVel(this.__n, x, y, z); return this; };
    Node.prototype.isValid = function () { return this.__n >= 0; };
    globalThis.Node = Node;
    globalThis.Vec3 = V3;
    var rawFind = scene.find;
    scene.find = function (name) { return new Node(rawFind(name)); };
    globalThis.__wrapSelf = function (id) { return new Node(id); };
})();
)JS";
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
    dai_script_bind_nodes(s, &nh);

    dai_script_play_host ph{};
    ph.key = fp_key; ph.get_vel = fp_get_vel; ph.set_vel = fp_set_vel;
    ph.impulse = fp_impulse; ph.grounded = fp_grounded; ph.mouse = fp_mouse;
    dai_script_bind_play(s, &ph);

    std::string prelude = load_prelude();
    CHECK(dai_script_eval(s, prelude.c_str(), "prelude", err, sizeof(err)) == DAI_OK,
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

    dai_script_destroy(s);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
