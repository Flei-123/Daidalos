// INNEN - a door that opens. One behaviour, on the LEAF itself.
//
// Put it on the door leaf: a box the size of the opening, with a collider, in
// the doorway. Everything else is inspector fields. The leaf swings around one
// vertical edge - its hinge - and the behaviour works out where that edge is
// from the leaf's own starting pose, so a door is placed like any other box
// and never needs a second "hinge" node dragged into place.
//
//   E           open / close, while standing within `range` of the leaf
//   locked      says no, and says it once
//
// WHY THE LEAF MOVES ITSELF: the runtime moves a node that has a body by
// setting the BODY's transform - children of that node do not follow, because
// nothing wrote the document. So a door is one node that is both the picture
// and the wall, and anything bolted to it (a handle) is moved by this script
// on purpose, by name. That is also why the leaf is KINEMATIC: a static body
// that teleports has no business in a solver, and a dynamic one falls over.
//
// @header Blatt
// @tooltip Width of the leaf in metres. The hinge sits half of this to the side.
// @param float width      = 0.95
// @tooltip Which edge the hinge is on: +1 the leaf's own +X side, -1 the -X side.
// @param float hinge      = 1
// @tooltip Yaw of the CLOSED leaf in degrees. 0 = the leaf lies across X, its
// @tooltip face looking down -Z. 90 = across Z, in a side wall.
// @param float yaw        = 0
// @tooltip How far it swings, in degrees. The sign is the direction it opens.
// @param float openAngle  = -85
// @tooltip Degrees per second while it moves.
// @param float speed      = 150
//
// @header Zustand
// @tooltip Open before anybody touches it.
// @param bool  startOpen  = false
// @tooltip Locked doors do not open. The game unlocks them, not the player.
// @param bool  locked     = false
// @tooltip Seconds after it finished opening before it shuts by itself. 0 = never.
// @param float autoClose  = 0
// @tooltip Slams shut the moment the player has crossed to the other side.
// @param bool  slamBehind = false
// @tooltip ...and is locked from then on. Only with slamBehind.
// @param bool  lockAfterSlam = false
// @tooltip Metres the player has to be PAST the door before it slams. A door
// @tooltip that fires the moment the plane is crossed closes onto the player
// @tooltip standing in its own frame and shoves him back into the room - which
// @tooltip is exactly what the first walk of this level did.
// @param float slamAfter  = 0.85
//
// @header Bedienung
// @tooltip Name of the node the player is on. It is what "within reach" measures to.
// @param string player    = Spieler
// @tooltip Metres. Further away than this, E does nothing.
// @param float range      = 2.2
// @tooltip Name of a node carried along with the leaf - a handle, a sign, a
// @tooltip number. Its offset is taken from where it stands at the start.
// @param string handle    =
//
// @header Test
// @tooltip Seconds after the level starts at which the door opens by itself,
// @tooltip unlocking first. 0 = off. tools/innen_walk.py uses this to prove
// @tooltip that a shut door stops the player and an open one does not.
// @param float autoOpen   = 0

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var DEG = Math.PI / 180;

var W, HINGE, YAW0, OPEN, SPEED, RANGE, AUTO_CLOSE, AUTO_OPEN;
var SLAM, LOCK_AFTER_SLAM, SLAM_AFTER;
var locked = false;
var angle = 0, target = 0;
var hx = 0, hy = 0, hz = 0;          // the hinge, in world space
var half = 0;                        // signed half width, hinge side
var PLAYER = -1, playerName = "Spieler";
var HANDLE = -1, handOff = [0, 0, 0];
var held = false, elapsed = 0, openedAt = -1;
var side0 = 0, slammed = false, autoFired = false;
var myName = "Tuer";

function rotY(deg, x, z) {
    var c = Math.cos(deg * DEG), s = Math.sin(deg * DEG);
    return [x * c + z * s, -x * s + z * c];
}

function quatY(deg) {
    var h = deg * DEG * 0.5;
    return [0, Math.sin(h), 0, Math.cos(h)];
}

// Where the leaf is when it stands at `a` degrees off closed: rotate the
// vector from the hinge to the centre, and put the centre there. Two lines
// instead of a parent node, and it is the same arithmetic the editor would do
// for a pivot.
function apply(a) {
    var abs = YAW0 + a;
    var d = rotY(abs, -half, 0);
    var px = hx + d[0], pz = hz + d[1];
    node.setPos(self, px, hy, pz);
    var q = quatY(abs);
    node.setRot(self, q[0], q[1], q[2], q[3]);
    if (HANDLE >= 0) {
        // handOff is stored in the CLOSED leaf's own frame, so it turns with
        // the leaf and nothing has to be re-measured while the door swings.
        var o = rotY(abs, handOff[0], handOff[2]);
        node.setPos(HANDLE, px + o[0], hy + handOff[1], pz + o[1]);
        node.setRot(HANDLE, q[0], q[1], q[2], q[3]);
    }
}

// Which side of the closed door's plane a point is on. The plane's normal is
// the closed leaf's facing, so this is +1 in front and -1 behind, and the sign
// flipping is the whole of "the player has gone through".
function signedDist(x, z) {
    var n = rotY(YAW0, 0, 1);
    return (x - hx) * n[0] + (z - hz) * n[1];
}

function sideOf(x, z) { return signedDist(x, z) >= 0 ? 1 : -1; }

function say(what) {
    print("DOOR t=" + (Math.round(elapsed * 100) / 100).toFixed(2) +
          " name=" + myName + " " + what +
          " a=" + (Math.round(angle * 10) / 10).toFixed(1) +
          (locked ? " locked" : ""));
}

function init() {
    W        = num("width", 0.95);
    HINGE    = num("hinge", 1) < 0 ? -1 : 1;
    YAW0     = num("yaw", 0);
    OPEN     = num("openAngle", -85);
    SPEED    = num("speed", 150);
    RANGE    = num("range", 2.2);
    AUTO_CLOSE = num("autoClose", 0);
    AUTO_OPEN  = num("autoOpen", 0);
    SLAM     = flag("slamBehind", false);
    LOCK_AFTER_SLAM = flag("lockAfterSlam", false);
    SLAM_AFTER = num("slamAfter", 0.85);
    locked   = flag("locked", false);
    playerName = text("player", "Spieler");
    myName   = node.getStr(self, "node.name") || ("Tuer" + self);

    var p = node.getPos(self);
    half = HINGE * W * 0.5;
    // The hinge edge, from where the leaf stands right now: half a width to
    // the hinge side, turned into the world by the closed yaw.
    var d = rotY(YAW0, half, 0);
    hx = p[0] + d[0]; hy = p[1]; hz = p[2] + d[1];

    var hn = text("handle", "");
    if (hn) {
        HANDLE = scene.find(hn);
        if (HANDLE >= 0) {
            var q = node.getPos(HANDLE);
            var o = rotY(-YAW0, q[0] - p[0], q[2] - p[2]);
            handOff = [o[0], q[1] - p[1], o[1]];
        }
    }

    if (flag("startOpen", false)) { angle = OPEN; target = OPEN; openedAt = 0; }
    apply(angle);

    PLAYER = scene.find(playerName);
    if (PLAYER >= 0) {
        var pp = node.getPos(PLAYER);
        side0 = sideOf(pp[0], pp[2]);
    }
    say("ready");
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    if (dt > 0.1) dt = 0.1;
    elapsed += dt;

    if (PLAYER < 0) PLAYER = scene.find(playerName);
    var near = false;
    if (PLAYER >= 0) {
        var pp = node.getPos(PLAYER);
        var lp = node.getPos(self);
        var dx = pp[0] - lp[0], dy = pp[1] - lp[1], dz = pp[2] - lp[2];
        near = (dx * dx + dy * dy + dz * dz) < RANGE * RANGE;

        // ---- the door that shuts behind you ------------------------------
        if (SLAM && !slammed) {
            var d = signedDist(pp[0], pp[2]);
            var s = d >= 0 ? 1 : -1;
            // ...and far enough past it. Closing on somebody still in the
            // frame is a door that pushes the player back where he came from.
            if (s !== side0 && Math.abs(d) > SLAM_AFTER) {
                slammed = true;
                target = 0;
                if (LOCK_AFTER_SLAM) locked = true;
                say("slam");
            }
        }
    }

    // ---- the key -----------------------------------------------------
    var e = input.key("e");
    if (e && !held && near) {
        if (locked) {
            say("refused");
        } else if (target === 0) {
            target = OPEN;
            say("opening");
        } else {
            target = 0;
            say("closing");
        }
    }
    held = e;

    // ---- the test's hand on the same door ----------------------------
    if (AUTO_OPEN > 0 && !autoFired && elapsed >= AUTO_OPEN) {
        autoFired = true;
        locked = false;
        target = OPEN;
        say("auto-opening");
    }

    // ---- the swing ---------------------------------------------------
    if (angle !== target) {
        var step = SPEED * dt;
        var diff = target - angle;
        if (Math.abs(diff) <= step) {
            angle = target;
            if (angle !== 0) openedAt = elapsed;
            say(angle === 0 ? "shut" : "open");
        } else {
            angle += (diff > 0 ? step : -step);
        }
        apply(angle);
    } else if (AUTO_CLOSE > 0 && target !== 0 && openedAt >= 0 &&
               elapsed - openedAt > AUTO_CLOSE) {
        target = 0;
        say("closing by itself");
    }
}
