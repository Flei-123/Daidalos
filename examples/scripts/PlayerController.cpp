// PlayerController.cpp - a player controller in C++, kept deliberately short.
//
// Drop this on an object that has a Rigidbody and press Ctrl+P. The editor
// compiles it the first time you play after it changed, loads it, and calls
// frame() sixty times a second. Change a number, press Play, see it: the same
// round trip the JavaScript behaviours have, with a compiler in the middle.
//
//   WASD / arrows   move
//   Space           jump
//   Shift           sprint
//
// WHY IT LOOKS LIKE THIS
//
// A behaviour is two functions and a struct. There is no base class to inherit
// from, no virtual anything, and no header of ours to include beyond this one:
// the engine hands you a table of function pointers (dai_native_api) and the
// id of the object you are on. That is the whole contract, and it is a C
// struct on purpose - a C++ base class would tie every behaviour to the exact
// compiler and standard library the editor was built with, and then the .dll
// you built on Tuesday would crash the editor on Wednesday.
//
// The fields the inspector shows are declared with "// @param" - the same
// spelling the .js behaviours use, because the editor reads the FILE, not the
// binary. A C++ member is not something an editor can see.
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

#include "dai_native.h"
#include <cmath>

// ---------------------------------------------------------------- state
//
// One instance of this struct per object carrying the script. Plain globals
// would be shared by every player in the scene, which is fine until there are
// two of them - and then it is a bug that takes an evening to find.
struct Player {
    float speed      = 6.0f;
    float sprintMul  = 1.7f;
    float jumpForce  = 5.5f;
    float airControl = 0.35f;
    bool  jumpHeld   = false;
};

static Player g_player;     // one object, one behaviour: see the note above

// ---------------------------------------------------------------- helpers

// "Is this key down." The engine speaks key CODES; a letter is its own code,
// which is why 'w' works directly.
static bool key(const dai_native_api *api, unsigned code) {
    return api->key_down(api, code) != 0;
}

// Standing on something? Not a raycast: a body that is neither rising nor
// sinking measurably is resting on whatever is under it, and that is exactly
// what a jump needs to know.
static bool grounded(const dai_native_api *api, dai_nentity self) {
    dai_nvec3 v = api->get_velocity(api, self);
    return v.y > -0.35f && v.y < 0.35f;
}

// ---------------------------------------------------------------- the behaviour

// Called once when Play starts.
DAI_BEHAVIOUR_INIT(api, self) {
    api->log(api, "PlayerController ready - WASD, Space, Shift");
    (void)self;
}

// Called every frame while the game runs. dt is the length of this frame.
DAI_BEHAVIOUR_FRAME(api, self, dt) {
    Player &p = g_player;

    // ---- what the keyboard is asking for, as a direction ----------------
    float ix = 0.0f, iz = 0.0f;
    if (key(api, 'a')) ix -= 1.0f;
    if (key(api, 'd')) ix += 1.0f;
    if (key(api, 'w')) iz -= 1.0f;
    if (key(api, 's')) iz += 1.0f;

    // Diagonals are not faster. Without this, holding W and D moves you 41%
    // quicker than holding W - the classic bug of every first controller.
    float len = std::sqrt(ix * ix + iz * iz);
    if (len > 0.0001f) { ix /= len; iz /= len; }

    float want = p.speed;
    if (key(api, DAI_KEY_SHIFT_L) || key(api, DAI_KEY_SHIFT_R)) want *= p.sprintMul;

    // ---- move by setting the horizontal velocity ------------------------
    // Keeping the Y component is what makes gravity still apply. A controller
    // that writes all three components every frame cannot fall.
    bool on_ground = grounded(api, self);
    dai_nvec3 v = api->get_velocity(api, self);
    float t = on_ground ? 1.0f : p.airControl;     // less authority in the air
    dai_nvec3 nv;
    nv.x = v.x + (ix * want - v.x) * t;
    nv.y = v.y;
    nv.z = v.z + (iz * want - v.z) * t;
    api->set_velocity(api, self, nv);

    // ---- jumping ---------------------------------------------------------
    // Edge triggered: held down, `space` is true every frame, and a jump that
    // fires every frame is a rocket.
    bool down = key(api, DAI_KEY_SPACE);
    if (down && !p.jumpHeld && on_ground) {
        dai_nvec3 up;
        up.x = 0.0f; up.y = p.jumpForce; up.z = 0.0f;
        api->add_impulse(api, self, up);
    }
    p.jumpHeld = down;

    (void)dt;   // this controller is velocity based, so it needs no dt
}
