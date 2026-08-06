// PlayerController.cpp - a player controller in C++, kept deliberately short.
//
// Drop this on an object that has a Rigidbody and press Ctrl+P.
//
//   WASD / arrows   move
//   Space           jump
//   Shift           sprint
//
// NEEDS A C++ COMPILER on PATH (g++ or clang++). The editor compiles this file
// the first time you press Play after it changed; with no compiler it says so
// in the Console and nothing happens. The .js behaviours need nothing.
//
// The numbers below are the inspector's - "// @param" lines are read from
// THIS FILE by the editor, drawn as fields, stored on the object, and handed
// back through me.param(). So the value written here is the default and the
// inspector is the truth, which is the same deal the .js behaviours have.
//
// @header Movement
// @tooltip Metres per second on the ground.
// @param float speed      = 6
// @tooltip Multiplied onto speed while Shift is held.
// @param float sprintMul  = 1.7
// @tooltip Upward impulse. Roughly: jump height in metres, times two.
// @param float jumpForce  = 5.5
// @tooltip How much steering is left while airborne, 0 to 1.
// @param float airControl = 0.35
// @header Camera
// @tooltip Optional: an object that follows this one from behind.
// @param camera followCam
// @param float camDistance = 8
// @param float camHeight   = 3

#include "dai_native.h"

// One flag that has to survive between frames - "was space already down last
// frame". Everything else is read fresh from the object each frame, which is
// what makes this file short.
static bool g_jump_held = false;

DAI_BEHAVIOUR_INIT(api, self) {
    Node me(api, self);
    me.log("PlayerController ready - WASD, Space, Shift");
}

DAI_BEHAVIOUR_FRAME(api, self, dt) {
    Node me(api, self);

    // ---- the inspector's numbers ----------------------------------------
    float speed      = me.param("speed", 6.0f);
    float sprintMul  = me.param("sprintMul", 1.7f);
    float jumpForce  = me.param("jumpForce", 5.5f);
    float airControl = me.param("airControl", 0.35f);

    // ---- what the keyboard is asking for ---------------------------------
    Vec3 dir;
    if (me.key('a') || me.key(DAI_KEY_LEFT))  dir.x -= 1;
    if (me.key('d') || me.key(DAI_KEY_RIGHT)) dir.x += 1;
    if (me.key('w') || me.key(DAI_KEY_UP))    dir.z -= 1;
    if (me.key('s') || me.key(DAI_KEY_DOWN))  dir.z += 1;
    // Diagonals are not faster. Without this, W and D together move you 41%
    // quicker than W alone - the classic bug of every first controller.
    dir = dir.normalised();

    float want = speed;
    if (me.key(DAI_KEY_SHIFT_L) || me.key(DAI_KEY_SHIFT_R)) want *= sprintMul;

    // ---- move by setting the horizontal velocity -------------------------
    // Keeping Y is what makes gravity still apply: a controller that writes
    // all three components every frame cannot fall.
    bool on_ground = me.grounded();
    Vec3 v = me.velocity();
    float t = on_ground ? 1.0f : airControl;      // less authority in the air
    me.velocity(Vec3(v.x + (dir.x * want - v.x) * t,
                     v.y,
                     v.z + (dir.z * want - v.z) * t));

    // ---- jumping ---------------------------------------------------------
    // Edge triggered: held down, space is true every frame, and a jump that
    // fires every frame is a rocket.
    bool down = me.key(DAI_KEY_SPACE);
    if (down && !g_jump_held && on_ground) me.impulse(Vec3(0, jumpForce, 0));
    g_jump_held = down;

    // ---- face the way we are going ---------------------------------------
    if (dir.length() > 0.01f) me.yaw(__builtin_atan2f(dir.x, dir.z) * 57.2957795f);

    // ---- the camera, if one was dragged into the field --------------------
    Node cam = me.param_node("followCam");
    if (cam) {
        Vec3 p = me.position();
        Vec3 goal(p.x, p.y + me.param("camHeight", 3.0f), p.z + me.param("camDistance", 8.0f));
        Vec3 c = cam.position();
        // Frame rate independent smoothing: close a fraction of the remaining
        // distance derived from a half life, not a fixed step per frame.
        float k = 1.0f - __builtin_powf(0.0001f, dt);
        cam.position(c + (goal - c) * k);
    }
}
