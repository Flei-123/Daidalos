// Player controller - the one script to test a scene with.
//
// Drop this on an object that has a Rigidbody (Add Component > Rigidbody) and
// press Play. WASD moves, Space jumps, Shift sprints.
//
// The lines starting with "@param" are this object's SERIALIZED FIELDS: the
// inspector draws a widget for each one and stores what you set on the NODE,
// not in this file. Two players can carry the same script with different
// speeds, and balancing the game never means editing code.
//
//   float / int  -> a number field
//   bool         -> a tick box
//   string       -> a text field
//   node         -> a slot you drag an object from the Hierarchy onto
//
// @param float  speed        = 6
// @param float  sprintMul    = 1.7
// @param float  jumpForce    = 5.5
// @param float  airControl   = 0.35
// @param int    maxJumps     = 2
// @param bool   faceMovement = true
// @param string playerName   = Player One
// @param node   followCam

// ---------------------------------------------------------------- globals
// `self`  - the node this script is attached to (the editor sets it).
// `state` - host values; state.dt is the length of this frame in seconds.
// `params`- what the inspector stored. Missing until something is assigned,
//           so everything below reads it through num()/flag()/text() and
//           falls back to the default written above.

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var SPEED, SPRINT, JUMP, AIR, MAXJUMPS, FACE, NAME, CAM;

var jumps = 0;
var jumpHeld = false;
var wasGrounded = true;

function init() {
    SPEED    = num("speed", 6);
    SPRINT   = num("sprintMul", 1.7);
    JUMP     = num("jumpForce", 5.5);
    AIR      = num("airControl", 0.35);
    MAXJUMPS = num("maxJumps", 2);
    FACE     = flag("faceMovement", true);
    NAME     = text("playerName", "Player One");
    // A node field arrives as the object's NAME; scene.find turns it into an
    // id. -1 when nothing is assigned, which is a perfectly good state.
    var camName = text("followCam", "");
    CAM = camName ? scene.find(camName) : -1;

    print(NAME + " ready on node " + self + "  (WASD, Space, Shift)");
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    var grounded = body.grounded(self);

    // ---- what the keyboard is asking for, as a direction on the ground ----
    var ix = 0, iz = 0;
    if (input.key("a") || input.key("left"))  ix -= 1;
    if (input.key("d") || input.key("right")) ix += 1;
    if (input.key("w") || input.key("up"))    iz -= 1;
    if (input.key("s") || input.key("down"))  iz += 1;
    var len = Math.sqrt(ix * ix + iz * iz);
    if (len > 0.0001) { ix /= len; iz /= len; }   // diagonals are not faster

    var want = SPEED * (input.key("shift") ? SPRINT : 1);

    // ---- move by setting the horizontal velocity, not by teleporting ------
    // Keeping the Y component is what makes gravity still apply: a controller
    // that writes all three every frame cannot fall.
    var v = body.getVel(self);
    var targetX = ix * want, targetZ = iz * want;
    var t = grounded ? 1.0 : AIR;                 // less authority in the air
    var nx = v[0] + (targetX - v[0]) * t;
    var nz = v[2] + (targetZ - v[2]) * t;
    body.setVel(self, nx, v[1], nz);

    // ---- jumping, with a real double jump ---------------------------------
    if (grounded && !wasGrounded) jumps = 0;      // landed: the count comes back
    wasGrounded = grounded;
    if (grounded) jumps = 0;

    var down = input.key("space");
    if (down && !jumpHeld && jumps < MAXJUMPS) {
        // Cancel whatever downward speed there was first, or a double jump
        // while falling barely lifts the object at all.
        var cur = body.getVel(self);
        body.setVel(self, cur[0], 0, cur[2]);
        body.impulse(self, 0, JUMP, 0);
        jumps += 1;
    }
    jumpHeld = down;

    // ---- turn to face where it is going ------------------------------------
    if (FACE && len > 0.0001) {
        // Yaw only, as a quaternion around Y: atan2 gives the angle, and half
        // of it is what a quaternion wants.
        var yaw = Math.atan2(ix, iz);
        var h = yaw * 0.5;
        node.setRot(self, 0, Math.sin(h), 0, Math.cos(h));
    }

    // ---- an assigned camera follows, if one was dragged into the field ----
    if (CAM >= 0) {
        var p = node.getPos(self);
        var c = node.getPos(CAM);
        var tx = p[0], ty = p[1] + 4.5, tz = p[2] + 8.0;
        var k = 1 - Math.pow(0.001, dt);          // frame rate independent lerp
        node.setPos(CAM, c[0] + (tx - c[0]) * k,
                         c[1] + (ty - c[1]) * k,
                         c[2] + (tz - c[2]) * k);
    }
}
