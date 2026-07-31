// Input actions: edges, axes, rebinding and persistence.
//
//   ./build/test_input [/tmp]

#include "dai_input.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::printf("input actions\n");

    dai_actions *a = dai_actions_create();
    dai_action jump = dai_action_define(a, "jump");
    dai_action fire = dai_action_define(a, "fire");
    dai_axis move = dai_axis_define(a, "move_x");

    CHECK(dai_action_define(a, "jump") == jump, "defining the same action twice made two handles");
    CHECK(dai_action_find(a, "fire") == fire, "find did not return the defined action");
    CHECK(dai_action_find(a, "nope") == DAI_INVALID_ACTION, "find invented an action");
    CHECK(!std::strcmp(dai_action_name(a, jump), "jump"), "action name round trip failed");

    dai_action_bind(a, jump, DAI_SRC_KEY(32));           // space
    dai_action_bind(a, jump, DAI_SRC_PAD(0));            // gamepad A
    dai_action_bind(a, fire, DAI_SRC_MOUSE(1));
    dai_axis_bind(a, move, DAI_SRC_KEY('a'), DAI_SRC_KEY('d'));

    // a key and a gamepad button must not collide even with the same number
    CHECK(DAI_SRC_KEY(0) != DAI_SRC_PAD(0) && DAI_SRC_KEY(1) != DAI_SRC_MOUSE(1),
          "source ranges overlap - a key could fire a gamepad action");

    // ---- edges: pressed fires once, held stays, released fires once
    dai_actions_update(a);
    CHECK(!dai_action_held(a, jump) && !dai_action_pressed(a, jump), "action started out active");

    dai_actions_source(a, DAI_SRC_KEY(32), 1);
    dai_actions_update(a);
    CHECK(dai_action_pressed(a, jump) && dai_action_held(a, jump), "press was not detected");
    dai_actions_update(a);
    CHECK(!dai_action_pressed(a, jump) && dai_action_held(a, jump), "pressed repeated while held");
    dai_actions_source(a, DAI_SRC_KEY(32), 0);
    dai_actions_update(a);
    CHECK(dai_action_released(a, jump) && !dai_action_held(a, jump), "release was not detected");
    dai_actions_update(a);
    CHECK(!dai_action_released(a, jump), "released repeated after the fact");

    // ---- the second binding drives the same action
    dai_actions_source(a, DAI_SRC_PAD(0), 1);
    dai_actions_update(a);
    CHECK(dai_action_held(a, jump), "the gamepad binding does not drive the action");
    dai_actions_source(a, DAI_SRC_PAD(0), 0);
    dai_actions_update(a);

    // ---- axis
    dai_actions_source(a, DAI_SRC_KEY('d'), 1);
    dai_actions_update(a);
    CHECK(fabsf(dai_axis_value(a, move) - 1.0f) < 1e-5f, "axis is %.2f with only + held", dai_axis_value(a, move));
    dai_actions_source(a, DAI_SRC_KEY('a'), 1);
    dai_actions_update(a);
    CHECK(fabsf(dai_axis_value(a, move)) < 1e-5f, "both directions held gave %.2f, expected 0", dai_axis_value(a, move));
    dai_actions_source(a, DAI_SRC_KEY('d'), 0);
    dai_actions_update(a);
    CHECK(fabsf(dai_axis_value(a, move) + 1.0f) < 1e-5f, "axis is %.2f with only - held", dai_axis_value(a, move));

    // analogue on the same axis, clamped
    dai_axis_bind_analog(a, move, DAI_SRC_PAD(16), 1.0f);
    dai_actions_analog(a, DAI_SRC_PAD(16), 0.5f);
    dai_actions_update(a);
    CHECK(fabsf(dai_axis_value(a, move) + 0.5f) < 1e-5f,
          "stick 0.5 plus key -1 gave %.2f, expected -0.5", dai_axis_value(a, move));
    dai_actions_source(a, DAI_SRC_KEY('a'), 0);
    dai_actions_analog(a, DAI_SRC_PAD(16), 3.0f);
    dai_actions_update(a);
    CHECK(fabsf(dai_axis_value(a, move) - 1.0f) < 1e-5f, "axis was not clamped: %.2f", dai_axis_value(a, move));

    // ---- rebinding: clear, then record a new key
    dai_action_clear(a, jump);
    dai_actions_source(a, DAI_SRC_KEY(32), 1);
    dai_actions_update(a);
    CHECK(!dai_action_held(a, jump), "the old binding still fires after clearing");
    dai_action_bind(a, jump, DAI_SRC_KEY('j'));
    dai_actions_source(a, DAI_SRC_KEY('j'), 1);
    dai_actions_update(a);
    CHECK(dai_action_held(a, jump), "the new binding does not fire");

    // ---- persistence
    std::string path = dir + "/bindings.cfg";
    CHECK(dai_actions_save(a, path.c_str()) == DAI_OK, "saving bindings failed");

    dai_actions *b = dai_actions_create();
    dai_action_define(b, "jump");
    dai_action_bind(b, dai_action_find(b, "jump"), DAI_SRC_KEY(999));   // a default to be replaced
    CHECK(dai_actions_load(b, path.c_str()) == DAI_OK, "loading bindings failed");
    CHECK(dai_action_count(b) == dai_action_count(a), "loaded %u actions, saved %u",
          dai_action_count(b), dai_action_count(a));
    CHECK(dai_axis_count(b) == dai_axis_count(a), "loaded %u axes, saved %u",
          dai_axis_count(b), dai_axis_count(a));

    dai_action bj = dai_action_find(b, "jump");
    dai_actions_source(b, DAI_SRC_KEY(999), 1);
    dai_actions_update(b);
    CHECK(!dai_action_held(b, bj), "the default binding survived loading - the file must replace it");
    dai_actions_source(b, DAI_SRC_KEY(999), 0);
    dai_actions_source(b, DAI_SRC_KEY('j'), 1);
    dai_actions_update(b);
    CHECK(dai_action_held(b, bj), "the loaded binding does not fire");

    dai_axis bm = dai_axis_find(b, "move_x");
    dai_actions_source(b, DAI_SRC_KEY('d'), 1);
    dai_actions_update(b);
    CHECK(fabsf(dai_axis_value(b, bm) - 1.0f) < 1e-5f, "loaded axis gives %.2f", dai_axis_value(b, bm));

    dai_actions_destroy(a);
    dai_actions_destroy(b);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
