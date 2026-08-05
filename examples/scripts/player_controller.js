// Third person player controller - the one script to test a scene with.
//
// Put it on an object that has a Rigidbody, drag your camera into followCam,
// and press Ctrl+P.
//
//   WASD / arrows   move, RELATIVE TO THE CAMERA - W is always "away from you"
//   Space           jump (double jump)
//   Shift           sprint
//   right mouse     look around; the camera orbits the player
//   wheel is free   - zoom lives in camDistance, so the inspector owns it
//
// The camera does two things a "follow" has to do and one it usually forgets:
// it moves to a point behind the player, it smooths that move, and it ALSO
// sets its own rotation to look at the player. A camera that only takes a
// position keeps pointing wherever it was left, which looks exactly like a
// camera that is not following at all.
//
// The lines starting with "@param" are this object's SERIALIZED FIELDS: the
// inspector draws a widget for each and stores what you set on the NODE, not
// in this file. Two players can carry this script with different speeds, and
// balancing the game never means editing code.
//
//   float / int  -> a number field
//   bool         -> a tick box
//   string       -> a text field
//   node         -> any object, dragged in or picked
//   camera/light/rigidbody/... -> the same slot, but only objects that HAVE
//                   that component: the declared type is the contract
//
// @header Movement
// @tooltip Metres per second on the ground.
// @param float  speed        = 6
// @tooltip Multiplied onto speed while Shift is held.
// @param float  sprintMul    = 1.7
// @param float  jumpForce    = 5.5
// @tooltip How much of the ground speed still steers while airborne, 0 to 1.
// @param float  airControl   = 0.35
// @param int    maxJumps     = 2
// @tooltip Turn the body to face the way it is moving.
// @param bool   faceMovement = true
// @param string playerName   = Player One
//
// @header Camera
// @tooltip The camera that follows this player. Only cameras can be dropped here.
// @param camera followCam
// @tooltip Metres behind the player.
// @param float  camDistance  = 8
// @tooltip Metres above the player's origin that the camera aims at.
// @param float  camHeight    = 1.6
// @tooltip Degrees per pixel of mouse movement.
// @param float  mouseSens    = 0.22
// @tooltip Hold the right button to look. Off means the mouse always looks.
// @param bool   holdToLook   = true
// @tooltip Higher is snappier. This is a half life, not a speed.
// @param float  camSmooth    = 12

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var SPEED, SPRINT, JUMP, AIR, MAXJUMPS, FACE, NAME;
var CAM, CAMDIST, CAMHEIGHT, SENS, HOLD, SMOOTH;

var jumps = 0, jumpHeld = false;
var yaw = 0, pitch = -12;          // degrees; pitch is clamped below
var camReady = false;              // first frame snaps, the rest smooth

var DEG = Math.PI / 180;

// Rotate a vector by a quaternion. v' = v + 2w(q x v) + 2(q x (q x v)).
function qrot(q, v) {
    var x = q[0], y = q[1], z = q[2], w = q[3];
    var tx = 2 * (y * v[2] - z * v[1]);
    var ty = 2 * (z * v[0] - x * v[2]);
    var tz = 2 * (x * v[1] - y * v[0]);
    return [v[0] + w * tx + (y * tz - z * ty),
            v[1] + w * ty + (z * tx - x * tz),
            v[2] + w * tz + (x * ty - y * tx)];
}

// Yaw about Y, then pitch about the new X - the only two angles a third
// person camera has. No roll, ever: a horizon that tilts is a bug report.
function yawPitchQuat(yawDeg, pitchDeg) {
    var cy = Math.cos(yawDeg * DEG * 0.5), sy = Math.sin(yawDeg * DEG * 0.5);
    var cp = Math.cos(pitchDeg * DEG * 0.5), sp = Math.sin(pitchDeg * DEG * 0.5);
    return [cy * sp, sy * cp, -sy * sp, cy * cp];
}

function init() {
    SPEED     = num("speed", 6);
    SPRINT    = num("sprintMul", 1.7);
    JUMP      = num("jumpForce", 5.5);
    AIR       = num("airControl", 0.35);
    MAXJUMPS  = num("maxJumps", 2);
    FACE      = flag("faceMovement", true);
    NAME      = text("playerName", "Player One");
    CAMDIST   = num("camDistance", 8);
    CAMHEIGHT = num("camHeight", 1.6);
    SENS      = num("mouseSens", 0.22);
    HOLD      = flag("holdToLook", true);
    SMOOTH    = num("camSmooth", 12);

    // A node field arrives as the object's NAME; scene.find turns it into an
    // id. -1 when nothing is assigned, which is a perfectly good state.
    var camName = text("followCam", "");
    CAM = camName ? scene.find(camName) : -1;

    print(NAME + " ready on node " + self +
          (CAM >= 0 ? "  camera: " + camName : "  (no camera assigned)") +
          "   WASD, Space, Shift, right mouse to look");
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    var grounded = body.grounded(self);

    // ---- looking -----------------------------------------------------------
    var looking = HOLD ? input.mouseButton(1) : true;
    if (looking) {
        yaw   -= input.mouseDX() * SENS;
        pitch -= input.mouseDY() * SENS;
        if (pitch > 75) pitch = 75;          // past vertical the basis flips
        if (pitch < -75) pitch = -75;
    }
    var camQ = yawPitchQuat(yaw, pitch);
    var fwd = qrot(camQ, [0, 0, -1]);

    // Movement is on the ground plane, so the camera's pitch must not shorten
    // the walk direction - flatten and renormalise instead of using fwd.
    var fl = Math.sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    var fx = fl > 0.0001 ? fwd[0] / fl : 0, fz = fl > 0.0001 ? fwd[2] / fl : -1;
    var rx = -fz, rz = fx;                   // right = forward x up

    // ---- what the keyboard is asking for, in world space --------------------
    var ax = 0, az = 0;
    if (input.key("w") || input.key("up"))    { ax += fx; az += fz; }
    if (input.key("s") || input.key("down"))  { ax -= fx; az -= fz; }
    if (input.key("d") || input.key("right")) { ax += rx; az += rz; }
    if (input.key("a") || input.key("left"))  { ax -= rx; az -= rz; }
    var len = Math.sqrt(ax * ax + az * az);
    if (len > 0.0001) { ax /= len; az /= len; }   // diagonals are not faster

    var want = SPEED * (input.key("shift") ? SPRINT : 1);

    // ---- move by setting the horizontal velocity, not by teleporting --------
    // Keeping the Y component is what makes gravity still apply: a controller
    // that writes all three every frame cannot fall.
    var v = body.getVel(self);
    var t = grounded ? 1.0 : AIR;                 // less authority in the air
    body.setVel(self, v[0] + (ax * want - v[0]) * t, v[1],
                      v[2] + (az * want - v[2]) * t);

    // ---- jumping, with a real double jump ----------------------------------
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

    // ---- turn to face where it is going -------------------------------------
    if (FACE && len > 0.0001) {
        var h = Math.atan2(ax, az) * 0.5;
        node.setRot(self, 0, Math.sin(h), 0, Math.cos(h));
    }

    // ---- the camera ---------------------------------------------------------
    if (CAM < 0) return;
    var p = node.getPos(self);
    var aim = [p[0], p[1] + CAMHEIGHT, p[2]];
    var goal = [aim[0] - fwd[0] * CAMDIST,
                aim[1] - fwd[1] * CAMDIST,
                aim[2] - fwd[2] * CAMDIST];

    var c = node.getPos(CAM);
    if (!camReady) { c = goal; camReady = true; }    // no swoop on the first frame
    // Frame rate independent smoothing: k is the fraction of the remaining
    // distance to close THIS frame, derived from a half life. Multiplying by
    // dt instead would make the camera stiffer at 30 fps than at 144.
    var k = 1 - Math.exp(-SMOOTH * dt);
    node.setPos(CAM, c[0] + (goal[0] - c[0]) * k,
                     c[1] + (goal[1] - c[1]) * k,
                     c[2] + (goal[2] - c[2]) * k);
    // ...and it LOOKS at the player. Without this the camera drifts along
    // behind pointing wherever the scene left it.
    node.setRot(CAM, camQ[0], camQ[1], camQ[2], camQ[3]);
}
