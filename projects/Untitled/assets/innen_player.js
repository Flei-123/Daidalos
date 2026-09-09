// INNEN - the player. First person, no jump, one torch.
//
// Put it on a capsule with a Rigidbody, drag the eye camera into `eye` and the
// spot light into `torch`. Every number below is an inspector field: the value
// here is the default, what you type on the node is the truth.
//
//   WASD        walk, relative to where you look
//   Shift       the fastest this game gets. It is not fast.
//   Ctrl        crouch - the ceiling in the box is 2.30 m and it feels it
//   F           torch on/off
//   mouse       look. No hold-to-look: this is first person.
//
// WHY NO JUMP: nothing in this game is solved by jumping, and a player who can
// jump will try to jump out of a room whose ceiling is the point. Stairs and
// door sills are climbed by the capsule, not by the space bar.
//
// @header Gehen
// @tooltip Metres per second. A walk, not a jog.
// @param float walkSpeed    = 2.2
// @tooltip Multiplied onto walkSpeed while Shift is held.
// @param float sprintMul    = 1.55
// @tooltip Multiplied onto walkSpeed while crouching.
// @param float crouchMul    = 0.45
// @tooltip How much steering is left with no ground under the feet, 0 to 1.
// @param float airControl   = 0.25
//
// @header Kopf
// @tooltip The camera that sits in this player's head. Only cameras fit here.
// @param camera eye
// @tooltip Metres from the capsule's CENTRE up to the eyes, standing. The
// @tooltip node sits in the middle of the capsule, not at the feet, because
// @tooltip that is where the physics body is.
// @param float eyeOffset    = 0.62
// @tooltip ...and crouched.
// @param float crouchOffset = 0.02
// @tooltip Degrees per pixel of mouse movement.
// @param float mouseSens    = 0.15
// @tooltip Centimetres the head rides up and down while walking. 0 is off.
// @param float headBob      = 2.4
//
// @header Taschenlampe
// @tooltip The spot light this player carries. Only lights fit here.
// @param light torch
// @tooltip Is it on when the level starts?
// @param bool  torchOn      = true
// @tooltip Brightness of a full battery.
// @param float torchPower   = 9
// @tooltip Minutes of light in a full battery. 0 = never runs out.
// @param float torchMinutes = 12
// @tooltip Below this fraction of the battery the light starts to stutter.
// @param float torchFlicker = 0.25
//
// @header Test
// @tooltip Walks forward by itself, with no keyboard. This is how
// @tooltip tools/innen_walk.py drives the player on a machine with no input.
// @param bool  autoWalk     = false
// @tooltip Degrees per second the automatic walk turns. 0 = straight ahead.
// @param float autoTurn     = 0

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var WALK, SPRINT, CROUCH, AIR;
var EYE = -1, EYE_H, CROUCH_H, SENS, BOB;
var TORCH = -1, TORCH_POWER, TORCH_MIN, TORCH_FLICK;
var AUTO, AUTO_TURN;

var yaw = 0, pitch = 0;          // 0 looks down -Z, which is where the door is
var torchOn = true, torchHeld = false;
var battery = 1.0;               // 1 = full, 0 = a dead torch and a dark hallway
var bobPhase = 0, headY = 0;
var elapsed = 0;

var DEG = Math.PI / 180;

function yawPitchQuat(yawDeg, pitchDeg) {
    var cy = Math.cos(yawDeg * DEG * 0.5), sy = Math.sin(yawDeg * DEG * 0.5);
    var cp = Math.cos(pitchDeg * DEG * 0.5), sp = Math.sin(pitchDeg * DEG * 0.5);
    return [cy * sp, sy * cp, -sy * sp, cy * cp];
}

// A tiny deterministic wobble for the dying torch. Not Math.random(): a
// flicker that differs between two runs makes a screenshot test impossible,
// and nobody can tell the difference by looking.
function wobble(t) {
    var s = Math.sin(t * 37.1) * 0.5 + Math.sin(t * 11.3) * 0.3 + Math.sin(t * 5.7) * 0.2;
    return s;
}

function init() {
    WALK     = num("walkSpeed", 2.2);
    SPRINT   = num("sprintMul", 1.55);
    CROUCH   = num("crouchMul", 0.45);
    AIR      = num("airControl", 0.25);
    EYE_H    = num("eyeOffset", 0.62);
    CROUCH_H = num("crouchOffset", 0.02);
    SENS     = num("mouseSens", 0.15);
    BOB      = num("headBob", 2.4) * 0.01;          // centimetres in the field
    TORCH_POWER = num("torchPower", 9);
    TORCH_MIN   = num("torchMinutes", 12);
    TORCH_FLICK = num("torchFlicker", 0.25);
    AUTO      = flag("autoWalk", false);
    AUTO_TURN = num("autoTurn", 0);
    torchOn   = flag("torchOn", true);

    var eyeName = text("eye", "");
    EYE = eyeName ? scene.find(eyeName) : -1;
    var torchName = text("torch", "");
    TORCH = torchName ? scene.find(torchName) : -1;

    // yaw 0 is -Z: forward = (-sin yaw, 0, -cos yaw), the same convention the
    // camera uses. Every room of INNEN is built along -Z and the box's door
    // faces that way, so this is "looking at the door" and not at the back
    // wall thirty centimetres behind your head.
    yaw = 0;
    headY = EYE_H;

    print("INNEN player on node " + self +
          (EYE >= 0 ? "  eye: " + eyeName : "  (NO CAMERA - the level will be seen from wherever)") +
          (TORCH >= 0 ? "  torch: " + torchName : "  (no torch)"));
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    if (dt > 0.1) dt = 0.1;                 // a hitch must not teleport anybody
    elapsed += dt;

    // ---- looking ---------------------------------------------------------
    yaw   -= input.mouseDX() * SENS;
    pitch -= input.mouseDY() * SENS;
    if (AUTO_TURN) yaw += AUTO_TURN * dt;
    if (pitch > 80) pitch = 80;
    if (pitch < -80) pitch = -80;

    var q = yawPitchQuat(yaw, pitch);
    // Walking direction is the flattened look direction: pitching down must
    // not shorten a step.
    var sy = Math.sin(yaw * DEG), cy = Math.cos(yaw * DEG);
    var fx = -sy, fz = -cy;                 // yaw 0 -> (0, 0, -1)
    var rx = -fz, rz = fx;

    // ---- what the player is asking for -----------------------------------
    var ax = 0, az = 0;
    if (AUTO) { ax = fx; az = fz; }
    if (input.key("w") || input.key("up"))    { ax += fx; az += fz; }
    if (input.key("s") || input.key("down"))  { ax -= fx; az -= fz; }
    if (input.key("d") || input.key("right")) { ax += rx; az += rz; }
    if (input.key("a") || input.key("left"))  { ax -= rx; az -= rz; }
    var len = Math.sqrt(ax * ax + az * az);
    if (len > 0.0001) { ax /= len; az /= len; }

    var crouching = input.key("ctrl") || input.key("control");
    var want = WALK * (crouching ? CROUCH : (input.key("shift") ? SPRINT : 1));

    // ---- move by velocity, never by teleporting --------------------------
    var grounded = body.grounded(self);
    var v = body.getVel(self);
    var t = grounded ? 1.0 : AIR;
    body.setVel(self, v[0] + (ax * want - v[0]) * t, v[1],
                      v[2] + (az * want - v[2]) * t);

    // ---- the head --------------------------------------------------------
    var target = crouching ? CROUCH_H : EYE_H;
    headY += (target - headY) * (1 - Math.exp(-14 * dt));
    var moving = len > 0.0001 && grounded;
    bobPhase += moving ? dt * (4.6 + want) : 0;
    if (!moving) bobPhase += dt * 0.6;                 // breathing, standing still
    var bob = Math.sin(bobPhase * 2.0) * BOB * (moving ? 1.0 : 0.25);

    var p = node.getPos(self);
    if (EYE >= 0) {
        node.setPos(EYE, p[0], p[1] + headY + bob, p[2]);
        node.setRot(EYE, q[0], q[1], q[2], q[3]);
    }

    // ---- the torch -------------------------------------------------------
    var f = input.key("f");
    if (f && !torchHeld) torchOn = !torchOn;
    torchHeld = f;

    if (TORCH >= 0) {
        if (torchOn && TORCH_MIN > 0) {
            battery -= dt / (TORCH_MIN * 60);
            if (battery < 0) battery = 0;
        }
        var lit = torchOn && battery > 0;
        // Brightness falls off with the battery, and the last quarter
        // stutters. The player is never told a number - they are told by the
        // room going dark.
        var level = lit ? (0.35 + 0.65 * battery) : 0;
        if (lit && battery < TORCH_FLICK && TORCH_FLICK > 0) {
            var s = wobble(elapsed) * (1 - battery / TORCH_FLICK);
            level *= (s > 0.55) ? 0.15 : 1.0;
        }
        node.setNum(TORCH, "light.intensity", TORCH_POWER * level);
        node.setNum(TORCH, "light.enabled", lit ? 1 : 0);
        // The torch is held, not worn: slightly right of the eye and a little
        // below it, so its cone shows the floor a step ahead.
        node.setPos(TORCH, p[0] + rx * 0.18, p[1] + headY - 0.14 + bob, p[2] + rz * 0.18);
        node.setRot(TORCH, q[0], q[1], q[2], q[3]);
    }
}
