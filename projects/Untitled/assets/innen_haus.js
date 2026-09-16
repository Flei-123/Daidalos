// INNEN - das Gehäuse. The rules of GDD §5.2, running in the shipped game.
//
// One behaviour on one node, "Haus", that watches which room the player is in
// and decides what the house is allowed to do behind his back:
//
//   WARM ROOMS      the last `warm` rooms he stood in are frozen, whatever
//                   else is true. The way back is always the way back.
//   COLD            a room he has not been within `coldDistance` doors of
//                   since he left it may change when he next walks in.
//   REBUILD         50 % nothing, 30 % a detail, 15 % the same kind of room
//                   again, 5 % something wrong. The last two BUILD a room
//                   while the game runs, with scene.spawn() - see
//                   docs/INNEN_HAUS.md. All of it happens now.
//   PINGPONG        walking A-B-A-B through the same door `pingpong` times is
//                   not answered with a reroll but with a REACTION: the door
//                   stops opening, or the lights go out. It is recording you,
//                   and it noticed you were testing it.
//   PULS            §5.4. While innen_puls.js says "dunkel", a room the player
//                   can SEE may be rebuilt too - but only outside the cone of
//                   his torch. In the bright phase, seeing a room protects it.
//   ANKER           §5.3. innen_anker.js writes the rooms that hold a real
//                   object into its own tag, and they join `anchors` - the
//                   map the dice below have always consulted. An anchored
//                   room is frozen whatever else is true, and picking the
//                   object back up gives the room to the house again.
//
// HOW IT SEES THE HOUSE: by name. A behaviour has no `editor` - it cannot
// enumerate the document - so examples/scripts/innen_gen.js names its rooms
// "Raum00".."RaumNN" with the kind in the node's tag, and its door leaves
// "Tuer00".."TuerMM" with "3>7" (the two rooms) in theirs. This reads those,
// and a room's rectangle comes out of its own floor slab: scale is
// (W + 2t, t, D + 2t), so the walls divide out.
//
// @header Das Haus
// @tooltip How many rooms the generator built. They are called Raum00, Raum01...
// @param float rooms       = 14
// @tooltip How many door leaves it built: Tuer00, Tuer01...
// @param float doors       = 24
// @tooltip The node the player is on.
// @param string player     = Spieler
//
// @header Regeln
// @tooltip How many recently visited rooms are frozen. 3 in the GDD.
// @param float warm        = 3
// @tooltip How many doors away he has to have been before a room goes cold.
// @param float coldDistance = 3
// @tooltip How many times through the same door before the house reacts.
// @param float pingpong    = 3
// @tooltip Rooms that never change, as indices: "0" is the phone box. An
// @tooltip anchor dropped in a room would add to this at play time.
// @param string anchors    = 0
// @tooltip The dice. The same seed makes the same house behave the same way.
// @param float seed        = 1
//
// @header Puls & Anker
// @tooltip The node carrying innen_puls.js. Its TAG is the phase - "hell",
// @tooltip "flacker" or "dunkel" - and that is the whole channel. Empty: the
// @tooltip house never goes dark and §5.4 is off.
// @param string puls       = Puls
// @tooltip The node carrying innen_anker.js. Its TAG is the list of rooms that
// @tooltip hold a real object, "0,3,7". Empty: only the fixed `anchors` above.
// @param string ankerNode  = Anker
// @tooltip Degrees of the torch's cone, §5.4. A room the beam falls into is
// @tooltip protected even in the dark. Has to match the spot light on the
// @tooltip player - innen_lib builds it at 26.
// @param float torchCone   = 26
// @tooltip Metres the torch reaches.
// @param float torchRange  = 16
// @tooltip The player's torch, for where the beam points.
// @param string torch      = Spieler.Lampe
//
// @header Test
// @tooltip Instead of watching the player, walk the graph `testSteps` times by
// @tooltip itself at startup and print what the rules did. That is how
// @tooltip tools/innen_haus.py measures the distribution in the SHIPPED game.
// @param bool  selfTest    = false
// @tooltip How many simulated moves.
// @param float testSteps   = 3000
// @tooltip How strongly the simulated walker goes back where it came from -
// @tooltip 0 never, 1 always. The pingpong rule has to be provoked to be seen.
// @param float testBounce  = 0.55
// @tooltip Report what the §5.4 sight rule answers for every room, once, from
// @tooltip where the player stands. Measured by tools/innen_puls.py.
// @param bool  sightTest   = false
// @tooltip Anchor this room for the first half of the self test and release it
// @tooltip for the second, counting rebuilds in both halves. -1 = off. This is
// @tooltip how tools/innen_anker.py proves §5.3 in the shipped runtime.
// @param float testAnchorRoom = -1

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var ROOMS = [];          // { id, node, name, type, cx, cz, w, d, lights[], doors[] }
var DOORS = [];          // { id, node, a, b, locked }
var ADJ = [];            // room -> [{ room, door }]
var PLAYER = -1;
var WARM, COLD_DIST, PINGPONG, SEED;
var anchors = {};

var here = -1;           // room the player is in
var trail = [];          // the warm list, newest first
var lastSeen = {};       // room -> "distance walked since we left it"
var since = {};          // room -> moves since it was left
var moves = 0;
var pingCount = {};      // door -> how often it was crossed back and forth
var lastDoor = -1, lastFrom = -1;
var elapsed = 0, nextLook = 0;
var dark = 0;            // seconds of forced darkness left
var PULS = -1, ANKER_NODE = -1, TORCH = -1;
var TORCH_COS = -1, TORCH_RANGE = 16;
var phase = "hell";      // what innen_puls.js last said
var lastAnkerTag = null;
var fixedAnchors = {};   // the ones from the inspector field, never removed

var stat = { enters: 0, identical: 0, detail: 0, wantReplace: 0, wantWrong: 0,
             builtReplace: 0, builtWrong: 0,
             frozen: 0, reactions: 0, lockReact: 0, darkReact: 0, flickerReact: 0,
             warmViolations: 0,
             // §5.4 / §5.3
             darkSeen: 0,        // rebuilt a room he could SEE, in the dark
             darkInCone: 0,      // ...and the ones the torch saved
             brightSeen: 0,      // seeing a room in the bright phase saved it
             anchorFrozen: 0,    // the dice never ran: a real object lies there
             anchorViolations: 0 };

// The same LCG the generator uses. Not Math.random(): a house that behaves
// differently in two runs of one seed cannot be tested and cannot be a bug
// report either.
var rngState = 1;
function rnd() {
    rngState = (Math.imul(rngState, 1664525) + 1013904223) >>> 0;
    return rngState / 4294967296;
}

function pad2(n) { return (n < 10 ? "0" : "") + n; }

// ---- reading the house out of the document ------------------------------
function readRooms(count) {
    for (var i = 0; i < count; i++) {
        var n = scene.find("Raum" + pad2(i));
        if (n < 0) continue;
        var floor = scene.find("Raum" + pad2(i) + ".Floor");
        var p = node.getPos(n);
        var w = 4, d = 4;
        if (floor >= 0) {
            var s = node.getVec(floor, "transform.scale");
            // scale is (W + 2t, t, D + 2t) - the slab is the room plus its
            // walls, so the walls divide back out.
            w = s[0] - 2 * s[1];
            d = s[2] - 2 * s[1];
        }
        var room = { id: i, node: n, name: "Raum" + pad2(i),
                     type: node.getStr(n, "node.tag"),
                     cx: p[0], cy: p[1], cz: p[2], w: w, d: d,
                     lights: [], doors: [], skin: skinOf(n) };
        // Its lights, by the generator's own naming.
        for (var l = 1; l <= 3; l++) {
            var li = scene.find(room.name + ".Licht." + l);
            if (li >= 0) room.lights.push({ node: li,
                                            base: node.getNum(li, "light.intensity") });
        }
        ROOMS.push(room);
        ADJ.push([]);
        since[i] = 999;
    }
}

function readDoors(count) {
    for (var i = 0; i < count; i++) {
        var n = scene.find("Tuer" + pad2(i));
        if (n < 0) continue;
        var tag = node.getStr(n, "node.tag") || "";
        var bits = tag.split(">");
        var a = parseInt(bits[0], 10);
        var b = parseInt(bits[1], 10);
        if (isNaN(a)) a = -1;
        if (isNaN(b)) b = -1;
        var door = { id: i, node: n, a: a, b: b, locked: false };
        DOORS.push(door);
        if (a >= 0 && b >= 0 && ADJ[a] && ADJ[b]) {
            ADJ[a].push({ room: b, door: DOORS.length - 1 });
            ADJ[b].push({ room: a, door: DOORS.length - 1 });
        }
        if (a >= 0 && ROOMS[a]) ROOMS[a].doors.push(DOORS.length - 1);
        if (b >= 0 && ROOMS[b]) ROOMS[b].doors.push(DOORS.length - 1);
    }
}

function roomAt(x, z) {
    for (var i = 0; i < ROOMS.length; i++) {
        var r = ROOMS[i];
        if (x > r.cx - r.w * 0.5 && x < r.cx + r.w * 0.5 &&
            z > r.cz - r.d * 0.5 && z < r.cz + r.d * 0.5) return i;
    }
    return -1;
}

// Graph distance, breadth first and capped: only "is it further than
// coldDistance" is ever asked, so there is no reason to walk the whole house.
function distance(from, to, cap) {
    if (from === to) return 0;
    var seen = {}, queue = [[from, 0]];
    seen[from] = 1;
    while (queue.length) {
        var cur = queue.shift();
        if (cur[1] >= cap) continue;
        var list = ADJ[cur[0]] || [];
        for (var i = 0; i < list.length; i++) {
            var nxt = list[i].room;
            if (seen[nxt]) continue;
            if (nxt === to) return cur[1] + 1;
            seen[nxt] = 1;
            queue.push([nxt, cur[1] + 1]);
        }
    }
    return cap + 1;
}

function isWarm(room) {
    for (var i = 0; i < trail.length && i < WARM; i++)
        if (trail[i] === room) return true;
    return false;
}

// ---- what the house is allowed to do -------------------------------------
function setLocked(doorIdx, on) {
    var d = DOORS[doorIdx];
    if (!d) return;
    d.locked = on;
    // The door behaviour reads its own tag every frame: "3>7" is the plain
    // one, a leading "!" means locked. A tag is a real field on the node, it
    // survives a save, and it is visible in the inspector - which is the whole
    // reason this and not a hidden side channel.
    node.setStr(d.node, "node.tag", (on ? "!" : "") + d.a + ">" + d.b);
}

function lightsOf(room, on, tint) {
    var r = ROOMS[room];
    if (!r) return;
    for (var i = 0; i < r.lights.length; i++) {
        var l = r.lights[i];
        node.setNum(l.node, "light.intensity", on ? l.base * tint : 0);
        node.setNum(l.node, "light.enabled", on ? 1 : 0);
    }
}

// ---- building a room while the game runs ---------------------------------
// A behaviour has no `editor` - it may not invent nodes - but since
// include/dai_spawn_host.inl it may COPY one an author placed:
//
//   scene.spawn(src, parent, name)   the whole subtree, components and all
//   scene.destroy(id)                the node and its descendants
//
// That is exactly the privilege this rule needs and no more. The 15 % and the
// 5 % were counted and logged for a while because the engine could not do
// this; now they are done.

// Every material the room's own parts wear, in child order. A room re-skinned
// with another room's skin is the same room in a different building - which
// is what "derselbe Raum nochmal" is supposed to feel like.
function skinOf(n) {
    var out = [], c = scene.childCount(n);
    for (var i = 0; i < c; i++) {
        var k = scene.childAt(n, i).valueOf();
        out.push(node.getStr(k, "material"));
    }
    return out;
}

function applySkin(n, skin) {
    if (!skin || !skin.length) return 0;
    var c = scene.childCount(n), used = 0;
    for (var i = 0; i < c; i++) {
        var m = skin[i % skin.length];
        if (!m) continue;
        node.setStr(scene.childAt(n, i).valueOf(), "material", m);
        used++;
    }
    return used;
}

// The copy carries the SOURCE's names - "Raum07.Floor" under a room that is
// now called Raum03. Renaming the subtree is not cosmetic: readRooms() finds a
// room's floor slab by "Raum03.Floor", and a house whose rooms answer under
// the wrong name is a house that measures the wrong rectangle.
function renameSubtree(n, fromPrefix, toPrefix) {
    var c = scene.childCount(n);
    for (var i = 0; i < c; i++) {
        var k = scene.childAt(n, i).valueOf();
        var nm = node.getStr(k, "node.name") || "";
        if (nm.indexOf(fromPrefix) === 0)
            node.setStr(k, "node.name", toPrefix + nm.substring(fromPrefix.length));
        renameSubtree(k, fromPrefix, toPrefix);
    }
}

// Tear the room down and put one up in its place, out of `src`. The order
// matters: the new one is standing before the old one comes down, because a
// spawn that is refused (a budget, a bad id) must leave the player in a room
// and not in the sky.
function replaceRoom(room, src, srcName, srcType, skin) {
    var r = ROOMS[room];
    if (!r) return "nothing";
    var made = scene.spawn(src, 0, r.name + ".neu").valueOf();
    if (made < 0) { say("spawn refused room=" + room); return "nothing"; }

    // Where the old one stood. Rooms are top level, so their own position IS
    // the world position - and the copy has to land on the doorways that are
    // already cut into the neighbours.
    node.setPos(made, r.cx, r.cy, r.cz);
    renameSubtree(made, srcName + ".", r.name + ".");
    if (skin) applySkin(made, skin);

    var old = r.node;
    node.setStr(made, "node.name", r.name);
    node.setStr(made, "node.tag", srcType);
    scene.destroy(old);
    r.node = made;
    r.type = srcType;
    r.skin = skinOf(made);
    return "built";
}

// 15 %: the same kind of room, built again. The source is the room ITSELF -
// which is the only source whose door holes are guaranteed to line up with the
// doors already hanging in the neighbouring walls - and it comes back wearing
// another room's surfaces. Same plan, different building.
function rebuildSame(room) {
    var r = ROOMS[room];
    if (!r || r.node < 0) return "nothing";
    var skin = null;
    for (var t = 0; t < 6; t++) {
        var o = ROOMS[Math.floor(rnd() * ROOMS.length)];
        if (o && o.id !== room && o.skin && o.skin.length) { skin = o.skin; break; }
    }
    return replaceRoom(room, r.node, r.name, r.type, skin);
}

// 5 %: something is wrong. Another room entirely is standing where this one
// was - so its door holes are in the wrong walls, its surfaces belong to a
// different part of the house, and the door you came through opens into a
// wall. That is the point: this is the roll that is allowed to be broken.
function rebuildWrong(room) {
    var r = ROOMS[room];
    if (!r) return "nothing";
    var best = null;
    for (var i = 0; i < ROOMS.length; i++) {
        var o = ROOMS[i];
        if (!o || o.id === room || o.node < 0) continue;
        // A room of roughly the same footprint stands in the hole; a much
        // bigger one would poke through the neighbours, which is not eerie,
        // it is a bug that looks like one.
        if (Math.abs(o.w - r.w) > 1.2 || Math.abs(o.d - r.d) > 1.2) continue;
        if (!best || rnd() < 0.5) best = o;
    }
    if (!best) return rebuildSame(room);
    return replaceRoom(room, best.node, best.name, best.type, null);
}

// 30 %: a detail is different. Everything here is something the engine can do
// to a room that already stands - the light is wrong, a lamp has moved, one
// door does not open any more.
function rebuildDetail(room) {
    var r = ROOMS[room];
    var what = rnd();
    if (r.lights.length && what < 0.45) {
        var li = r.lights[Math.floor(rnd() * r.lights.length)];
        node.setNum(li.node, "light.intensity", li.base * (0.25 + rnd() * 0.5));
        node.setVec(li.node, "light.color", 1.0, 0.72 + rnd() * 0.18, 0.55 + rnd() * 0.2);
        return "light";
    }
    if (r.lights.length && what < 0.75) {
        var lm = r.lights[Math.floor(rnd() * r.lights.length)];
        var p = node.getPos(lm.node);
        node.setPos(lm.node, p[0] + (rnd() - 0.5) * r.w * 0.5, p[1],
                             p[2] + (rnd() - 0.5) * r.d * 0.5);
        return "moved";
    }
    if (r.doors.length > 1) {
        // "eine Tür weniger": one of its doors stops working. Never the one he
        // just came through - that is the warm room rule in door form.
        for (var t = 0; t < 4; t++) {
            var di = r.doors[Math.floor(rnd() * r.doors.length)];
            if (di === lastDoor) continue;
            setLocked(di, true);
            return "door";
        }
    }
    return "nothing";
}

function say(line) { print("HAUS " + line); }

// ---- §5.4: what the pulse changes ---------------------------------------
// The phase is read off innen_puls.js's own tag, the same way innen_door.js
// reads the tag this file writes to it. One channel for the whole game.
function readPhase() {
    if (PULS < 0) return "hell";
    var t = node.getStr(PULS, "node.tag");
    return (t === "dunkel" || t === "flacker") ? t : "hell";
}

// Can the player SEE into this room? Deliberately crude and deliberately
// generous: the room's centre against the player's own position and facing.
// A frustum-and-occlusion test is what §5.2's "kein Sichtkontakt" will
// eventually mean (docs/GDD_INNEN.md §10), and it needs a renderer. What
// matters for the RULE is the asymmetry: in the bright phase, a room you are
// looking at is safe, and in the dark that protection shrinks to the beam.
function playerLook() {
    if (PLAYER < 0) return null;
    var p = node.getPos(PLAYER);
    // The torch is aimed where the head is aimed - innen_player.js writes the
    // eye's rotation onto it every frame - so the beam IS the look direction,
    // and taking it from the torch means there is one source for both.
    var f = [0, 0, -1];
    if (TORCH >= 0) {
        var q = node.getRot(TORCH);
        if (q && q.length === 4) {
            var x = q[0], y = q[1], z = q[2], w = q[3];
            // rotate (0,0,-1) by q
            f = [-2 * (x * z + w * y),
                 -2 * (y * z - w * x),
                 -(1 - 2 * (x * x + y * y))];
        }
    }
    return { x: p[0], y: p[1], z: p[2], fx: f[0], fy: f[1], fz: f[2] };
}

// Is the room's centre inside the torch's cone, and near enough to be lit?
// This is the ONE thing that is safe during a dark phase - "alles aus ausser
// deiner Taschenlampe", and the house may rebuild everything the beam is not
// on. A player who wants a room to stay put has to keep the light on it.
function inTorchCone(room, look) {
    if (!look || TORCH < 0) return false;
    if (!node.getNum(TORCH, "light.enabled")) return false;   // a dead torch protects nothing
    var r = ROOMS[room];
    if (!r) return false;
    var dx = r.cx - look.x, dy = r.cy - look.y, dz = r.cz - look.z;
    var len = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (len > TORCH_RANGE) return false;
    if (len < 0.0001) return true;
    var dot = (dx * look.fx + dy * look.fy + dz * look.fz) / len;
    return dot >= TORCH_COS;
}

// "Sichtkontakt" for the bright phase: the room is in front of the player and
// within a few rooms' reach. Same crude measure, wider angle - the eye is not
// a torch.
function inSight(room, look) {
    if (!look) return false;
    var r = ROOMS[room];
    if (!r) return false;
    var dx = r.cx - look.x, dy = r.cy - look.y, dz = r.cz - look.z;
    var len = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (len > TORCH_RANGE * 1.5) return false;
    if (len < 0.0001) return true;
    var dot = (dx * look.fx + dy * look.fy + dz * look.fz) / len;
    return dot >= 0.5;                       // a 60 degree half angle
}

// ---- §5.3: the anchors the player carries --------------------------------
// innen_anker.js owns the objects; this owns the rule. Its tag is the list of
// rooms that hold one, and it is read into the SAME `anchors` map the rebuild
// dice have always consulted - so an anchored room is frozen by the rule that
// was already there, not by a second one bolted next to it.
function readAnchors() {
    if (ANKER_NODE < 0) return;
    var t = node.getStr(ANKER_NODE, "node.tag");
    if (t === null || t === undefined || t === lastAnkerTag) return;
    lastAnkerTag = t;
    anchors = {};
    for (var k in fixedAnchors)
        if (fixedAnchors.hasOwnProperty(k)) anchors[k] = 1;
    var bits = String(t).split(",");
    for (var i = 0; i < bits.length; i++) {
        var v = parseInt(bits[i], 10);
        if (!isNaN(v)) anchors[v] = 1;
    }
    var list = [];
    for (var a in anchors) if (anchors.hasOwnProperty(a)) list.push(a);
    say("anchors now " + list.join(","));
}

// The whole rule, in one place: what happens when the player walks into a
// room. Called by frame() when he really does, and by the self test thousands
// of times so the distribution can be measured in the shipped game.
//
// `simulated` is true for the self test and false for the real walk, and it
// gates exactly one thing: the §5.4 sight rule. The self test walks the GRAPH
// - the player's capsule never moves - so asking "can he see this room from
// where he is standing" would answer about the spawn point four thousand
// times and silently freeze whichever rooms lie in front of it. Everything
// else - warm rooms, anchors, the dice, pingpong - is the same code on both
// paths, because a rules test against a different rule proves nothing.
function enterRoom(room, viaDoor, simulated) {
    moves++;
    stat.enters++;

    // pingpong: the same door, back and forth
    if (viaDoor >= 0) {
        if (viaDoor === lastDoor) {
            pingCount[viaDoor] = (pingCount[viaDoor] || 0) + 1;
        } else {
            pingCount[viaDoor] = 1;
        }
        if (pingCount[viaDoor] >= PINGPONG) {
            react(viaDoor, room);
            pingCount[viaDoor] = 0;
        }
        lastDoor = viaDoor;
    }

    var wasWarm = isWarm(room);
    var away = since[room] === undefined ? 999 : since[room];

    // the trail, newest first
    for (var i = trail.length - 1; i >= 0; i--) if (trail[i] === room) trail.splice(i, 1);
    trail.unshift(room);
    if (trail.length > 16) trail.pop();
    since[room] = 0;
    for (var k in since) if (since.hasOwnProperty(k) && +k !== room) since[k]++;

    // §5.3 first, and before anything else: a room with something REAL in it
    // is not the house's to change. That is the whole mechanic - the house
    // cannot overwrite what it did not build.
    if (anchors[room]) { stat.frozen++; stat.anchorFrozen++; return "anchor"; }
    if (wasWarm) { stat.frozen++; return "warm"; }
    if (away < COLD_DIST) { stat.frozen++; return "near"; }

    // §5.4: seeing a room is what protects it - and in the dark phase that
    // protection shrinks from "wherever you look" to "wherever the beam is".
    // The room he is walking INTO is the one being rolled for, so this is the
    // rule that makes a dark phase feel different from a bright one: keep the
    // torch on the doorway and the room stays; look away and it may not.
    var look = simulated ? null : playerLook();
    if (look && inSight(room, look)) {
        if (phase !== "dunkel") { stat.frozen++; stat.brightSeen++; return "seen"; }
        if (inTorchCone(room, look)) { stat.frozen++; stat.darkInCone++; return "cone"; }
        stat.darkSeen++;                     // seen, dark, outside the beam: fair game
    }

    var roll = rnd();
    if (roll < 0.50) { stat.identical++; return "identical"; }
    if (roll < 0.80) {
        var what = rebuildDetail(room);
        stat.detail++;
        return "detail:" + what;
    }
    if (roll < 0.95) {
        stat.wantReplace++;
        var same = rebuildSame(room);
        if (same === "built") stat.builtReplace++;
        return "replace:" + same;
    }
    stat.wantWrong++;
    var wrong = rebuildWrong(room);
    if (wrong === "built") stat.builtWrong++;
    return "wrong:" + wrong;
}

// The house noticed. Weighted the way §5.2 weights it, with the two reactions
// that need a Kopie (a double of the player) folded into darkness for now.
function react(door, room) {
    stat.reactions++;
    var r = rnd();
    if (r < 0.40) {
        setLocked(door, true);
        stat.lockReact++;
        say("reaction=lock door=" + door + " room=" + room);
        return "lock";
    }
    if (r < 0.75) {
        dark = 12 + rnd() * 20;
        for (var i = 0; i < ROOMS.length; i++) lightsOf(i, false, 1);
        stat.darkReact++;
        say("reaction=dark door=" + door + " room=" + room);
        return "dark";
    }
    lightsOf(room, true, 0.25);
    stat.flickerReact++;
    say("reaction=flicker door=" + door + " room=" + room);
    return "flicker";
}

// ---- the self test -------------------------------------------------------
// It walks the graph instead of the player, with a bias for turning round, and
// then prints the numbers tools/innen_haus.py checks. It runs the SAME
// enterRoom() the game runs - a rules test against a copy of the rules would
// prove nothing.
function selfTest(steps, bounce) {
    var cur = 0, prevDoor = -1, prev = -1;
    var warmChecked = 0;

    // §5.3, measured rather than asserted: one room is anchored for the FIRST
    // half of the walk and released for the second. The promise is not "the
    // dice are kind to it" - it is that the dice never run at all while the
    // object lies there, and that the room goes back to being the house's the
    // moment it is picked up. Both halves are counted below.
    var probe = num("testAnchorRoom", -1);
    var half = Math.floor(steps / 2);
    var anchoredRebuilds = 0, releasedRebuilds = 0, releasedVisits = 0;
    if (probe >= 0) {
        anchors[probe] = 1;
        say("test anchoring room " + probe + " for the first " + half + " moves");
    }

    for (var i = 0; i < steps; i++) {
        if (probe >= 0 && i === half) {
            // The object is picked up again. Nothing else changes.
            delete anchors[probe];
            if (fixedAnchors[probe]) anchors[probe] = 1;
            say("test releasing room " + probe + " at move " + i);
        }
        var list = ADJ[cur] || [];
        if (!list.length) { cur = 0; prev = -1; prevDoor = -1; continue; }
        var goBack = prev >= 0 && rnd() < bounce;
        var choice = null;
        if (goBack) {
            for (var j = 0; j < list.length; j++)
                if (list[j].room === prev) choice = list[j];
        }
        if (!choice) choice = list[Math.floor(rnd() * list.length)];

        // What the rule promises: a warm room is the same room. Checked BEFORE
        // the move, by remembering what the answer has to be.
        var mustFreeze = isWarm(choice.room) || anchors[choice.room] ||
                         (since[choice.room] !== undefined && since[choice.room] < COLD_DIST);
        var answer = enterRoom(choice.room, choice.door, true);
        if (probe >= 0 && choice.room === probe) {
            var changed = (answer.indexOf("detail") === 0 ||
                           answer.indexOf("replace") === 0 ||
                           answer.indexOf("wrong") === 0);
            if (i < half) {
                if (changed) { anchoredRebuilds++; stat.anchorViolations++;
                               say("ANCHOR VIOLATION room=" + probe +
                                   " answer=" + answer); }
            } else {
                releasedVisits++;
                if (changed) releasedRebuilds++;
            }
        }
        if (mustFreeze) {
            warmChecked++;
            if (answer !== "warm" && answer !== "anchor" && answer !== "near") {
                stat.warmViolations++;
                say("VIOLATION room=" + choice.room + " answer=" + answer);
            }
        }
        prev = cur;
        prevDoor = choice.door;
        cur = choice.room;
    }
    if (probe >= 0)
        say("anchor probe room=" + probe + " anchoredRebuilds=" + anchoredRebuilds +
            " releasedVisits=" + releasedVisits +
            " releasedRebuilds=" + releasedRebuilds);
    say("test steps=" + steps + " enters=" + stat.enters +
        " frozen=" + stat.frozen + " warmChecked=" + warmChecked +
        " identical=" + stat.identical + " detail=" + stat.detail +
        " wantReplace=" + stat.wantReplace + " wantWrong=" + stat.wantWrong +
        " builtReplace=" + stat.builtReplace + " builtWrong=" + stat.builtWrong +
        " reactions=" + stat.reactions +
        " lock=" + stat.lockReact + " dark=" + stat.darkReact +
        " flicker=" + stat.flickerReact +
        " violations=" + stat.warmViolations +
        " anchorFrozen=" + stat.anchorFrozen +
        " anchorViolations=" + stat.anchorViolations +
        " brightSeen=" + stat.brightSeen + " darkSeen=" + stat.darkSeen +
        " darkInCone=" + stat.darkInCone);
}

// ---- the §5.4 self test --------------------------------------------------
// The dice test above walks the graph with no player. This one does the
// opposite: it does not walk at all, it puts the player somewhere, points the
// torch at a known room, and asks the sight rule what it answers - in the
// bright phase and in the dark one. That is the only way to see the asymmetry
// the GDD is actually asking for:
//
//   hell    a room you can see is safe.
//   dunkel  a room you can see is safe ONLY if the beam is on it.
//
// It reports one line per room, and tools/innen_puls.py reads it.
function sightTest() {
    if (PLAYER < 0 || !ROOMS.length) { say("sight test: no player"); return; }
    var p = node.getPos(PLAYER);
    var savedPhase = phase;

    // THE BEAM POINTS ONE WAY. An earlier version of this test aimed the torch
    // at each room in turn and then asked whether that room was in the cone -
    // which every room within range obviously was. It measured the torch's
    // RANGE and called it the cone, and it would have passed just as happily
    // with the cone set to 180 degrees.
    //
    // What §5.4 actually promises is a CHOICE: the beam is somewhere, and the
    // rooms it is not on are the ones the house may rebuild while you watch.
    // So the torch is aimed ONCE - at the nearest room within its reach - and
    // every other room is judged against that one direction, which is what the
    // game does to them.
    // Which room to point at: the one that leaves the most house in front of
    // the player, because a beam aimed into a corner measures nothing. Every
    // room within reach is tried as an aim point and the one that puts the
    // most OTHER rooms in view wins - that is the situation §5.4 is about,
    // standing in a corridor with rooms ahead of you and one torch.
    var aim = null, aimRoom = -1, bestSeen = -1;
    for (var a = 0; a < ROOMS.length; a++) {
        var ra = ROOMS[a];
        var ax = ra.cx - p[0], ay = ra.cy - p[1], az = ra.cz - p[2];
        var al = Math.sqrt(ax * ax + ay * ay + az * az);
        if (al < 0.5 || al > TORCH_RANGE) continue;
        var cand = { x: p[0], y: p[1], z: p[2],
                     fx: ax / al, fy: ay / al, fz: az / al };
        var n = 0;
        for (var b = 0; b < ROOMS.length; b++) if (inSight(b, cand)) n++;
        if (n > bestSeen) { bestSeen = n; aimRoom = a; aim = cand; }
    }
    if (!aim) { say("sight test: no room within torch range"); return; }

    var seen = 0, coneSaved = 0, exposed = 0, blind = 0;
    for (var i = 0; i < ROOMS.length; i++) {
        var r = ROOMS[i];
        var dx = r.cx - p[0], dy = r.cy - p[1], dz = r.cz - p[2];
        var len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.0001) continue;
        // Both questions against the SAME fixed look direction.
        var sight = inSight(i, aim);
        var cone = inTorchCone(i, aim);
        if (sight) seen++; else blind++;
        if (sight && cone) coneSaved++;
        if (sight && !cone) exposed++;
        say("sight room=" + i + " dist=" + len.toFixed(2) +
            " seen=" + (sight ? 1 : 0) + " cone=" + (cone ? 1 : 0));
    }
    phase = savedPhase;
    say("sight rooms=" + ROOMS.length + " aimedAt=" + aimRoom +
        " seen=" + seen + " blind=" + blind +
        " coneSaved=" + coneSaved + " exposed=" + exposed +
        " torch=" + (TORCH >= 0 ? 1 : 0));
}

function init() {
    WARM = num("warm", 3);
    COLD_DIST = num("coldDistance", 3);
    PINGPONG = num("pingpong", 3);
    SEED = num("seed", 1);
    rngState = (SEED >>> 0) || 1;

    readRooms(num("rooms", 14));
    readDoors(num("doors", 24));

    var list = text("anchors", "0").split(",");
    for (var i = 0; i < list.length; i++) {
        var v = parseInt(list[i], 10);
        // Kept separately as well: innen_anker.js rewrites `anchors` whenever
        // an object is picked up, and the rooms the LEVEL declared anchored
        // (the phone box) must survive that.
        if (!isNaN(v)) { anchors[v] = 1; fixedAnchors[v] = 1; }
    }

    PLAYER = scene.find(text("player", "Spieler"));

    // §5.4 and §5.3: the two nodes this one listens to. Both are optional -
    // a floor without a Puls never goes dark, and one without an Anker node
    // has only the anchors its level declared.
    var pn = text("puls", "Puls");
    PULS = pn ? scene.find(pn) : -1;
    var an = text("ankerNode", "Anker");
    ANKER_NODE = an ? scene.find(an) : -1;
    var tn = text("torch", "Spieler.Lampe");
    TORCH = tn ? scene.find(tn) : -1;
    TORCH_RANGE = num("torchRange", 16);
    // Half angle, as a cosine, compared once per test instead of an acos per
    // room per move.
    TORCH_COS = Math.cos(num("torchCone", 26) * 0.5 * Math.PI / 180);

    say("ready rooms=" + ROOMS.length + " doors=" + DOORS.length +
        " warm=" + WARM + " cold=" + COLD_DIST + " pingpong=" + PINGPONG +
        " puls=" + (PULS >= 0 ? "yes" : "no") +
        " ankerNode=" + (ANKER_NODE >= 0 ? "yes" : "no") +
        " torch=" + (TORCH >= 0 ? "yes" : "no"));

    if (flag("selfTest", false))
        selfTest(num("testSteps", 3000), num("testBounce", 0.55));
    if (flag("sightTest", false)) sightTest();
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    elapsed += dt;

    if (dark > 0) {
        dark -= dt;
        if (dark <= 0) {
            for (var i = 0; i < ROOMS.length; i++) lightsOf(i, true, 1);
            say("light returns");
        }
    }

    // Four times a second is plenty to notice a doorway, and it keeps the
    // rectangle test off the frame budget in a house of forty rooms.
    if (elapsed < nextLook) return;
    nextLook = elapsed + 0.25;

    // The two channels, on the same beat as the doors': what phase the pulse
    // is in, and which rooms hold something real.
    var was = phase;
    phase = readPhase();
    if (phase !== was) say("phase=" + phase);
    readAnchors();
    if (PLAYER < 0) { PLAYER = scene.find(text("player", "Spieler")); return; }

    var p = node.getPos(PLAYER);
    var r = roomAt(p[0], p[2]);
    if (r < 0 || r === here) return;

    // Which door joins the two, if any - that is what the pingpong counter
    // counts, and standing in a doorway must not count as a move.
    var via = -1, list = ADJ[here] || [];
    for (var j = 0; j < list.length; j++) if (list[j].room === r) via = list[j].door;
    var was = here;
    here = r;
    var answer = enterRoom(r, via, false);
    say("enter room=" + r + " (" + (ROOMS[r] ? ROOMS[r].type : "?") + ")" +
        " from=" + was + " door=" + via + " -> " + answer);
}
