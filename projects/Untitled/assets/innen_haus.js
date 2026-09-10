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
//                   again, 5 % something wrong. The last two need rooms to be
//                   BUILT at play time and this engine cannot do that yet, so
//                   they are drawn, counted and logged as `want=replace` -
//                   see docs/INNEN_HAUS.md. The 80 % that can happen, happens.
//   PINGPONG        walking A-B-A-B through the same door `pingpong` times is
//                   not answered with a reroll but with a REACTION: the door
//                   stops opening, or the lights go out. It is recording you,
//                   and it noticed you were testing it.
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

var stat = { enters: 0, identical: 0, detail: 0, wantReplace: 0, wantWrong: 0,
             frozen: 0, reactions: 0, lockReact: 0, darkReact: 0, flickerReact: 0,
             warmViolations: 0 };

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
                     cx: p[0], cz: p[2], w: w, d: d, lights: [], doors: [] };
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

// The whole rule, in one place: what happens when the player walks into a
// room. Called by frame() when he really does, and by the self test thousands
// of times so the distribution can be measured in the shipped game.
function enterRoom(room, viaDoor) {
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

    if (anchors[room]) { stat.frozen++; return "anchor"; }
    if (wasWarm) { stat.frozen++; return "warm"; }
    if (away < COLD_DIST) { stat.frozen++; return "near"; }

    var roll = rnd();
    if (roll < 0.50) { stat.identical++; return "identical"; }
    if (roll < 0.80) {
        var what = rebuildDetail(room);
        stat.detail++;
        return "detail:" + what;
    }
    if (roll < 0.95) {
        // 15 %: the same kind of room, built again. Needs a room to be built
        // while the game runs, which needs a spawn API this engine does not
        // have yet. Counted, logged, not faked.
        stat.wantReplace++;
        return "want=replace";
    }
    stat.wantWrong++;
    return "want=wrong";
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
    for (var i = 0; i < steps; i++) {
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
        var answer = enterRoom(choice.room, choice.door);
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
    say("test steps=" + steps + " enters=" + stat.enters +
        " frozen=" + stat.frozen + " warmChecked=" + warmChecked +
        " identical=" + stat.identical + " detail=" + stat.detail +
        " wantReplace=" + stat.wantReplace + " wantWrong=" + stat.wantWrong +
        " reactions=" + stat.reactions +
        " lock=" + stat.lockReact + " dark=" + stat.darkReact +
        " flicker=" + stat.flickerReact +
        " violations=" + stat.warmViolations);
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
        if (!isNaN(v)) anchors[v] = 1;
    }

    PLAYER = scene.find(text("player", "Spieler"));
    say("ready rooms=" + ROOMS.length + " doors=" + DOORS.length +
        " warm=" + WARM + " cold=" + COLD_DIST + " pingpong=" + PINGPONG);

    if (flag("selfTest", false))
        selfTest(num("testSteps", 3000), num("testBounce", 0.55));
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
    var answer = enterRoom(r, via);
    say("enter room=" + r + " (" + (ROOMS[r] ? ROOMS[r].type : "?") + ")" +
        " from=" + was + " door=" + via + " -> " + answer);
}
