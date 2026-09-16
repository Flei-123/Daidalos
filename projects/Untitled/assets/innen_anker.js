// INNEN - Anker. GDD §5.3 and the inventory of §5.9, in the shipped game.
//
// Five things out of your car: Warndreieck, Erste-Hilfe-Box, Eiskratzer,
// Tankquittung, Foto. They are the only objects in the house that are REAL -
// they came from outside, they were not built out of somebody's memory - and
// the house cannot overwrite what is real. Put one down in a room and that
// room never changes again. Pick it up and the room is the house's again.
//
//   E        pick up what you are standing next to / put down what you carry
//   1-4      choose a jacket pocket
//
// THE INVENTORY IS PHYSICAL, §5.9: the torch is in your hand and does not use
// a pocket. The jacket has 4 slots. An anchor is big and takes TWO of them -
// so you can carry two anchors and nothing else, or one anchor and two small
// things. There is no menu and there is no crafting.
//
// HOW A ROOM GETS FROZEN: not by a second copy of the freeze rule. The rule
// lives in innen_haus.js, which already has an `anchors` map that the rebuild
// dice consult before every roll - the phone box has been in it from the
// start. This behaviour writes into THAT map through the channel the house
// already uses for everything else: a tag. This node's tag is the list of
// anchored room indices, "0,3,7", and innen_haus.js reads it four times a
// second and updates its own map. One rule, one place, one owner.
//
// WHY THE ANCHORS ARE NOT SPAWNED HERE: a behaviour has no `editor` and may
// not invent nodes. The level script places the five objects (innen_lib's
// `anker()`), this behaviour only ever moves them, hides them and shows them.
//
// @header Haus
// @tooltip How many rooms the generator built: Raum00, Raum01...
// @param float rooms       = 14
// @tooltip The node the player is on.
// @param string player     = Spieler
// @tooltip The node carrying innen_haus.js. Its `anchors` field is seeded from
// @tooltip this node's tag, so the two never hold different lists.
// @param string haus       = Haus
//
// @header Gegenstaende
// @tooltip The five anchors, by node name, in the order the GDD lists them.
// @tooltip They have to exist in the scene - a behaviour may not create them.
// @tooltip Separated by '|': the document stores a script's fields as
// @tooltip "file.js{a=1,b=2}" and splits them on the COMMA, so a list that
// @tooltip used commas would arrive as four broken fields. That is not a
// @tooltip style choice, it is the only separator that survives the format.
// @param string items      = Anker.Warndreieck|Anker.ErsteHilfe|Anker.Eiskratzer|Anker.Tankquittung|Anker.Foto
// @tooltip Metres. Closer than this to an anchor, E picks it up.
// @param float reach       = 1.8
// @tooltip Metres in front of the player a dropped anchor lands.
// @param float dropAhead   = 0.9
//
// @header Inventar (§5.9)
// @tooltip Pockets in the jacket. The torch is in the hand and needs none.
// @param float slots       = 4
// @tooltip How many pockets one anchor fills. They are big.
// @param float ankerSlots  = 2
//
// @header Test
// @tooltip Instead of watching the player, run the scripted sequence in
// @tooltip `testPlan` at startup and print what the rules answered. That is
// @tooltip how tools/innen_anker.py measures this in the SHIPPED game.
// @param bool  selfTest    = false
// @tooltip A '|' separated programme: "take:0" take anchor 0, "drop:0@3"
// @tooltip drop anchor 0 in room 3, "check" print the state. '|' and not ','
// @tooltip because the script field format splits its own fields on commas.
// @param string testPlan   =

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var ROOMS = [];          // { id, cx, cz, w, d } - the same rectangles the house uses
var ITEMS = [];          // { id, name, node, room, carried, big }
var PLAYER = -1;
var SLOTS, ANKER_SLOTS, REACH, DROP_AHEAD;
var carried = [];        // indices into ITEMS
var elapsed = 0, nextLook = 0, held = false;
var lastTag = null;

var stat = { takes: 0, drops: 0, refusedFull: 0, refusedFar: 0, refusedNothing: 0 };

function say(line) { print("ANKER " + line); }
function pad2(n) { return (n < 10 ? "0" : "") + n; }

// ---- the house, as rectangles -------------------------------------------
// The same derivation innen_haus.js makes: a room's floor slab is scaled
// (W + 2t, t, D + 2t), so the walls divide back out. Two readings of one
// geometry have to agree, and they agree because they are the same three
// lines - "which room is this point in" must mean exactly one thing.
function readRooms(count) {
    for (var i = 0; i < count; i++) {
        var n = scene.find("Raum" + pad2(i));
        if (n < 0) continue;
        var floor = scene.find("Raum" + pad2(i) + ".Floor");
        var p = node.getPos(n);
        var w = 4, d = 4;
        if (floor >= 0) {
            var s = node.getVec(floor, "transform.scale");
            w = s[0] - 2 * s[1];
            d = s[2] - 2 * s[1];
        }
        ROOMS.push({ id: i, cx: p[0], cy: p[1], cz: p[2], w: w, d: d });
    }
}

function roomAt(x, z) {
    for (var i = 0; i < ROOMS.length; i++) {
        var r = ROOMS[i];
        if (x > r.cx - r.w * 0.5 && x < r.cx + r.w * 0.5 &&
            z > r.cz - r.d * 0.5 && z < r.cz + r.d * 0.5) return r.id;
    }
    return -1;
}

// ---- the inventory, §5.9 -------------------------------------------------
function usedSlots() {
    var n = 0;
    for (var i = 0; i < carried.length; i++) n += ITEMS[carried[i]].big;
    return n;
}

function freeSlots() { return SLOTS - usedSlots(); }

function isCarried(idx) {
    for (var i = 0; i < carried.length; i++) if (carried[i] === idx) return true;
    return false;
}

// ---- the channel to the house -------------------------------------------
// The anchored rooms as a tag: "0,3,7". innen_haus.js reads it and updates the
// map its rebuild dice already consult. The phone box (room 0) is anchored
// from the start by the generator, and stays in the list whatever happens to
// the five objects - it is not one of them.
function writeTag() {
    var list = ["0"];
    for (var i = 0; i < ITEMS.length; i++) {
        var it = ITEMS[i];
        if (!it.carried && it.room >= 0 && it.room !== 0) list.push(String(it.room));
    }
    var tag = list.join(",");
    if (tag === lastTag) return;
    node.setStr(self, "node.tag", tag);
    lastTag = tag;
    say("anchored=" + tag);
}

// ---- taking and putting down --------------------------------------------
function take(idx) {
    var it = ITEMS[idx];
    if (!it || it.carried) return "nothing";
    if (freeSlots() < it.big) {
        stat.refusedFull++;
        say("refused=full item=" + it.name + " needs=" + it.big +
            " free=" + freeSlots());
        return "full";
    }
    it.carried = true;
    var wasRoom = it.room;
    it.room = -1;
    carried.push(idx);
    // Out of the world: it is in a pocket now, and a pocket is not a place
    // the renderer has to draw. The node stays - a behaviour may not destroy
    // what an author placed and then be expected to give it back.
    if (it.node >= 0) {
        node.setNum(it.node, "renderer.enabled", 0);
        node.setNum(it.node, "rigidbody.enabled", 0);
    }
    stat.takes++;
    writeTag();
    say("take item=" + it.name + " fromRoom=" + wasRoom +
        " slots=" + usedSlots() + "/" + SLOTS);
    return "took";
}

function drop(idx, x, y, z, room) {
    var it = ITEMS[idx];
    if (!it || !it.carried) return "nothing";
    it.carried = false;
    it.room = room;
    for (var i = carried.length - 1; i >= 0; i--)
        if (carried[i] === idx) carried.splice(i, 1);
    if (it.node >= 0) {
        node.setPos(it.node, x, y, z);
        node.setNum(it.node, "renderer.enabled", 1);
    }
    stat.drops++;
    writeTag();
    say("drop item=" + it.name + " room=" + room +
        " slots=" + usedSlots() + "/" + SLOTS);
    return "dropped";
}

// The nearest anchor lying on the floor within reach, or -1.
function nearest(px, py, pz) {
    var best = -1, bestD = REACH * REACH;
    for (var i = 0; i < ITEMS.length; i++) {
        var it = ITEMS[i];
        if (it.carried || it.node < 0) continue;
        var p = node.getPos(it.node);
        var dx = p[0] - px, dy = p[1] - py, dz = p[2] - pz;
        var d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD) { bestD = d2; best = i; }
    }
    return best;
}

function init() {
    SLOTS       = num("slots", 4);
    ANKER_SLOTS = num("ankerSlots", 2);
    REACH       = num("reach", 1.8);
    DROP_AHEAD  = num("dropAhead", 0.9);

    readRooms(num("rooms", 14));

    // '|' is the real separator (see the field's tooltip); ',' is still
    // accepted so a hand written field behaves the way it looks.
    var names = text("items", "").split(/[|,]/);
    for (var i = 0; i < names.length; i++) {
        var nm = names[i].replace(/^\s+|\s+$/g, "");
        if (!nm) continue;
        var n = scene.find(nm);
        var room = -1;
        if (n >= 0) {
            var p = node.getPos(n);
            room = roomAt(p[0], p[2]);
        }
        ITEMS.push({ id: ITEMS.length, name: nm, node: n, room: room,
                     carried: false, big: ANKER_SLOTS });
    }

    PLAYER = scene.find(text("player", "Spieler"));
    var found = 0;
    for (var k = 0; k < ITEMS.length; k++) if (ITEMS[k].node >= 0) found++;
    say("ready items=" + ITEMS.length + " found=" + found +
        " rooms=" + ROOMS.length + " slots=" + SLOTS +
        " ankerSlots=" + ANKER_SLOTS);
    writeTag();

    if (flag("selfTest", false)) selfTest(text("testPlan", ""));
}

// ---- the self test -------------------------------------------------------
// It runs the SAME take()/drop() the game runs, driven by a written
// programme, and prints the state after each step. tools/innen_anker.py
// reads those lines and then asks the HOUSE, in the same run, whether the
// room it anchored really stopped being rebuilt.
function selfTest(plan) {
    // Split on '|' for the same reason the item list does: the document
    // stores a script's fields as "file.js{a=1,b=2}" and splits them on the
    // COMMA, so a programme written with commas arrives as six broken fields
    // and only its first step ever runs. That is exactly what happened.
    var steps = plan.split(/[|]/);
    for (var i = 0; i < steps.length; i++) {
        var s = steps[i].replace(/^\s+|\s+$/g, "");
        if (!s) continue;
        if (s.indexOf("take:") === 0) {
            var ti = parseInt(s.substring(5), 10);
            var answer = take(ti);
            say("step=" + i + " take " + ti + " -> " + answer +
                " used=" + usedSlots() + " free=" + freeSlots());
        } else if (s.indexOf("drop:") === 0) {
            var body = s.substring(5);
            var at = body.split("@");
            var di = parseInt(at[0], 10);
            var dr = parseInt(at[1], 10);
            var r = null;
            for (var q = 0; q < ROOMS.length; q++) if (ROOMS[q].id === dr) r = ROOMS[q];
            var answer2 = r ? drop(di, r.cx, r.cy + 0.2, r.cz, dr) : "noroom";
            say("step=" + i + " drop " + di + " in " + dr + " -> " + answer2 +
                " used=" + usedSlots() + " free=" + freeSlots());
        } else if (s === "check") {
            var list = [];
            for (var m = 0; m < ITEMS.length; m++)
                list.push(ITEMS[m].name + (ITEMS[m].carried ? ":hand" :
                          ":room" + ITEMS[m].room));
            say("step=" + i + " check used=" + usedSlots() + "/" + SLOTS +
                " carried=" + carried.length + " [" + list.join(" ") + "]");
        }
    }
    say("test takes=" + stat.takes + " drops=" + stat.drops +
        " refusedFull=" + stat.refusedFull +
        " used=" + usedSlots() + " slots=" + SLOTS +
        " ankerSlots=" + ANKER_SLOTS + " items=" + ITEMS.length);
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    elapsed += dt;
    if (PLAYER < 0) { PLAYER = scene.find(text("player", "Spieler")); return; }

    var e = input.key("e");
    var pressed = e && !held;
    held = e;
    if (!pressed) return;

    var p = node.getPos(PLAYER);
    var near = nearest(p[0], p[1], p[2]);
    if (near >= 0) { take(near); return; }

    // Nothing to pick up: put the last thing taken down, a step in front,
    // in whatever room the player is standing in. Dropping an anchor into no
    // room at all would anchor nothing and lose the object in a corridor the
    // house does not own.
    if (!carried.length) { stat.refusedNothing++; return; }
    var idx = carried[carried.length - 1];
    var room = roomAt(p[0], p[2]);
    if (room < 0) { stat.refusedFar++; say("refused=noroom"); return; }
    drop(idx, p[0], p[1] - 0.35, p[2] - DROP_AHEAD, room);
}
