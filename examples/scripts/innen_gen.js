// INNEN — the room generator. "Wachstum statt Karte" (GDD §5.1), as data.
//
// Sent down the bridge behind the library, with its options in front:
//
//     tools/daibridge.py js -f examples/scripts/innen_lib.js \
//                           -f examples/scripts/innen_gen.js
//     # options, if any:  var GEN = { seed: 7, rooms: 14 };
//
// It works in TWO halves, and that split is the whole point of the file:
//
//   plan(seed, n)   pure arithmetic. Rooms, sizes, doorways, the graph. No
//                   editor call, no node, nothing that needs a document. A
//                   layout is a value, so it can be computed twice and
//                   compared, which is what makes "the same seed is the same
//                   floor" a test instead of a hope.
//   build(plan)     turns that value into rooms with INNEN.room() - the same
//                   function examples/scripts/innen_m0.js uses by hand, so a
//                   generated door and a hand placed one are the same doorway.
//
// The contract it grows along is the one M0 established: a room exposes door
// SOCKETS, and a door is two sockets at the same point in the world with
// opposite normals and the same opening. The generator never invents geometry;
// it picks a socket and puts a room on it.
//
// What it does NOT do yet: the rebuild rule (§5.2), warm rooms, anti-pingpong,
// anchors, portals. Those are about a room CHANGING while the player is in the
// house, and there is no house to change until this file exists.

(function () {
    if (typeof INNEN === "undefined")
        throw new Error("innen_gen.js needs innen_lib.js in front of it");

    var OPT = (typeof GEN === "object" && GEN) ? GEN : {};
    var SEED = (typeof OPT.seed === "number") ? OPT.seed : 1;
    var WANT = (typeof OPT.rooms === "number") ? OPT.rooms : 14;
    var WITH_PLAYER = (OPT.player === false) ? false : true;
    var PLAN_ONLY = OPT.planOnly === true;

    var M = INNEN.M, has = INNEN.has;

    // ---- the dice ---------------------------------------------------------
    // A linear congruential generator, written out: Math.random() would make
    // every run a different house and every failure impossible to look at
    // twice. Numbers from Numerical Recipes; the top bits are the good ones,
    // so the value is taken from the high end.
    function Rng(seed) {
        this.s = (seed >>> 0) || 1;
    }
    Rng.prototype.next = function () {
        this.s = (Math.imul(this.s, 1664525) + 1013904223) >>> 0;
        return this.s / 4294967296;
    };
    Rng.prototype.range = function (a, b) { return a + (b - a) * this.next(); };
    Rng.prototype.int = function (a, b) {          // inclusive
        return a + Math.floor(this.next() * (b - a + 1));
    };
    Rng.prototype.pick = function (list) { return list[this.int(0, list.length - 1)]; };
    // Weighted draw: list of [thing, weight].
    Rng.prototype.weighted = function (table) {
        var total = 0, i;
        for (i = 0; i < table.length; i++) total += table[i][1];
        var r = this.next() * total;
        for (i = 0; i < table.length; i++) {
            r -= table[i][1];
            if (r <= 0) return table[i][0];
        }
        return table[table.length - 1][0];
    };

    // ---- the modules ------------------------------------------------------
    // Every room is a box with a size RANGE, so two hallways are not the same
    // hallway. `doors` is how many ways out it wants besides the one it came
    // in through - a 0 makes a dead end, which the GDD asks for at about 15 %.
    var DOOR_W = 0.95, DOOR_H = 2.02;
    var TYPES = {
        Vorraum:     { w: [1.30, 1.30], d: [1.30, 1.30], h: [2.30, 2.30], t: 0.12,
                       doors: [1, 1], mats: "metal", lights: 1 },
        Flur:        { w: [1.60, 2.10], d: [5.00, 11.00], h: [2.45, 2.65], t: 0.15,
                       doors: [2, 4], mats: "home", lights: 3 },
        Zimmer:      { w: [3.20, 4.60], d: [3.00, 4.40], h: [2.45, 2.70], t: 0.15,
                       doors: [0, 3], mats: "home", lights: 1 },
        Kammer:      { w: [1.40, 2.20], d: [1.40, 2.10], h: [2.20, 2.40], t: 0.12,
                       doors: [0, 1], mats: "home", lights: 1 },
        Nasszelle:   { w: [1.80, 2.60], d: [1.80, 3.00], h: [2.30, 2.50], t: 0.12,
                       doors: [0, 1], mats: "wet", lights: 1 },
        Treppenhaus: { w: [2.60, 3.40], d: [3.40, 4.60], h: [3.60, 4.40], t: 0.18,
                       doors: [1, 2], mats: "concrete", lights: 2 },
        Halle:       { w: [7.00, 10.00], d: [6.00, 9.00], h: [3.80, 4.60], t: 0.20,
                       doors: [1, 3], mats: "home", lights: 2 },
        Maschine:    { w: [4.00, 6.00], d: [4.00, 6.00], h: [3.00, 3.80], t: 0.20,
                       doors: [1, 2], mats: "concrete", lights: 2 }
    };

    // The zone table of GDD §5.1: what is likely at which distance from the
    // phone box. Zone 0 is the flat you remember, zone 3 is the building it
    // never was.
    var ZONES = [
        [["Flur", 6], ["Zimmer", 3]],                                   // 1
        [["Zimmer", 5], ["Flur", 4], ["Kammer", 2], ["Nasszelle", 2]],  // 2
        [["Flur", 4], ["Zimmer", 4], ["Nasszelle", 2], ["Treppenhaus", 2],
         ["Kammer", 2], ["Halle", 1]],                                  // 3
        [["Treppenhaus", 3], ["Halle", 3], ["Flur", 3], ["Maschine", 2],
         ["Kammer", 2]],                                                // 4
        [["Halle", 3], ["Maschine", 3], ["Treppenhaus", 2], ["Flur", 2]] // 5+
    ];

    function zoneTable(depth) {
        return ZONES[Math.min(depth, ZONES.length) - 1] || ZONES[ZONES.length - 1];
    }

    var MATS = {
        metal:    { floor: M.metal, wall: M.metal, ceiling: M.metal },
        home:     { floor: M.floor, wall: M.wall, ceiling: M.wall },
        wet:      { floor: M.floor, wall: M.wall, ceiling: M.ceiling },
        concrete: { floor: M.floor, wall: M.wall, ceiling: M.ceiling }
    };

    var OPPOSITE = { front: "back", back: "front", left: "right", right: "left" };
    var NORMAL = { front: [0, 0, -1], back: [0, 0, 1], left: [-1, 0, 0], right: [1, 0, 0] };

    // Where a room's centre has to be so that its `side` wall's opening at
    // `at` lands exactly on the world point `world`. The arithmetic every dock
    // in this file goes through, in ONE place.
    function centreFor(dim, side, at, world) {
        var halfW = dim.width * 0.5 + dim.wall * 0.5;
        var halfD = dim.depth * 0.5 + dim.wall * 0.5;
        if (side === "front") return [world[0] - at, 0, world[2] + halfD];
        if (side === "back")  return [world[0] - at, 0, world[2] - halfD];
        if (side === "left")  return [world[0] + halfW, 0, world[2] - at];
        return [world[0] - halfW, 0, world[2] - at];         // right
    }

    // The world point of an opening at `at` on `side` of a room at `centre`.
    function socketWorld(centre, dim, side, at) {
        var halfW = dim.width * 0.5 + dim.wall * 0.5;
        var halfD = dim.depth * 0.5 + dim.wall * 0.5;
        if (side === "front") return [centre[0] + at, 0, centre[2] - halfD];
        if (side === "back")  return [centre[0] + at, 0, centre[2] + halfD];
        if (side === "left")  return [centre[0] - halfW, 0, centre[2] + at];
        return [centre[0] + halfW, 0, centre[2] + at];       // right
    }

    // A room's footprint - the INSIDE of it, as [x0, x1, z0, z1].
    //
    // Not including the walls, and that is deliberate: two rooms that share a
    // doorway also share that wall's line, so their wall slabs overlap by half
    // a thickness each. That is how M0 is built by hand as well. What may
    // never overlap is the space a player stands in, so the space a player
    // stands in is what is measured.
    function foot(centre, dim) {
        var hx = dim.width * 0.5;
        var hz = dim.depth * 0.5;
        return [centre[0] - hx, centre[0] + hx, centre[2] - hz, centre[2] + hz];
    }

    function overlaps(a, b, eps) {
        return a[0] < b[1] - eps && b[0] < a[1] - eps &&
               a[2] < b[3] - eps && b[2] < a[3] - eps;
    }

    function pad2(n) { return (n < 10 ? "0" : "") + n; }

    // ------------------------------------------------------------------ plan
    function plan(seed, want) {
        var rng = new Rng(seed);
        var rooms = [], edges = [], open = [];
        var refused = { overlap: 0, noSpace: 0 };

        function addRoom(type, centre, dim, depth) {
            var r = {
                index: rooms.length,
                // The NAME is a data channel, the way the door sockets' names
                // already are - a behaviour at play time has no editor to
                // enumerate the document with, it can only look things up by
                // name. So a room is "Raum03", findable from its index alone,
                // and what KIND of room it is goes in the node's tag.
                name: "Raum" + pad2(rooms.length),
                type: type, centre: centre, dim: dim, depth: depth,
                doors: { front: [], back: [], left: [], right: [] },
                foot: foot(centre, dim)
            };
            rooms.push(r);
            return r;
        }

        function addOpening(room, side, at, w, h) {
            room.doors[side].push({ at: at, width: w, height: h });
            return {
                room: room.index, side: side,
                index: room.doors[side].length - 1,
                at: at, width: w, height: h,
                world: socketWorld(room.centre, room.dim, side, at),
                normal: NORMAL[side]
            };
        }

        // Rooms are built on a 60 cm module, the way a real floor plan is.
        // Not for tidiness: with free sizes two rooms NEVER end up sharing a
        // wall line, and a door that comes out in a room already standing -
        // the loop of GDD §5.1 - can then only happen by accident, which is
        // to say never. On a module they line up constantly.
        var MOD = 0.60;
        function snap(v, lo, hi) {
            var m = Math.round(v / MOD) * MOD;
            if (m < lo) m = Math.ceil(lo / MOD) * MOD;
            if (m > hi) m = Math.floor(hi / MOD) * MOD;
            return Math.round(m * 100) / 100;
        }
        // What is snapped is the WALL LINE SPAN - centre wall to centre wall -
        // and not the inner size, because the wall line is what a neighbour
        // meets. A room's half span is then always a multiple of 30 cm, so
        // every room in the house sits on one grid however its walls are
        // dimensioned, and two of them line up constantly.
        function sizeOf(type, rng) {
            var T = TYPES[type];
            var spanW = snap(rng.range(T.w[0], T.w[1]) + T.t, T.w[0] + T.t, T.w[1] + T.t);
            var spanD = snap(rng.range(T.d[0], T.d[1]) + T.t, T.d[0] + T.t, T.d[1] + T.t);
            return {
                width:  Math.round((spanW - T.t) * 100) / 100,
                depth:  Math.round((spanD - T.t) * 100) / 100,
                height: Math.round(rng.range(T.h[0], T.h[1]) * 20) / 20,
                wall:   T.t
            };
        }

        // Where on a wall an opening may sit: never in a corner, so a door
        // frame is never half a wall.
        function slot(span, w, rng) {
            var room = span * 0.5 - w * 0.5 - 0.35;
            if (room <= 0) return 0;
            // On the same 30 cm half module as the rooms, for the same reason.
            var v = rng.range(-room, room);
            var q = Math.round(v / (MOD * 0.5)) * (MOD * 0.5);
            if (q < -room) q += MOD * 0.5;
            if (q > room) q -= MOD * 0.5;
            return Math.round(q * 100) / 100;
        }

        // ---- joining onto a room that is already there --------------------
        // GDD §5.1 asks for about 10 % of new doors to come out in an EXISTING
        // room. It is tried the moment an opening is made, not once at the
        // end: a socket that is still open when the growth stops is out at the
        // edge of the house, where there is nothing to join onto, so an end of
        // run pass finds almost nothing.
        var loops = 0;

        function wallLine(room, side) {
            var c = room.centre, d = room.dim;
            if (side === "front") return { axis: "z", at: c[2] - (d.depth * 0.5 + d.wall * 0.5),
                                           lo: c[0] - d.width * 0.5, hi: c[0] + d.width * 0.5 };
            if (side === "back")  return { axis: "z", at: c[2] + (d.depth * 0.5 + d.wall * 0.5),
                                           lo: c[0] - d.width * 0.5, hi: c[0] + d.width * 0.5 };
            if (side === "left")  return { axis: "x", at: c[0] - (d.width * 0.5 + d.wall * 0.5),
                                           lo: c[2] - d.depth * 0.5, hi: c[2] + d.depth * 0.5 };
            return { axis: "x", at: c[0] + (d.width * 0.5 + d.wall * 0.5),
                     lo: c[2] - d.depth * 0.5, hi: c[2] + d.depth * 0.5 };
        }

        // Does this socket land on the wall of a room that already stands? If
        // it does, cut the matching opening on the other side and call it a
        // door. Nothing is moved to make it fit - either the two walls are the
        // same line or they are not.
        function tryJoin(s0) {
            var facing = OPPOSITE[s0.side];
            for (var ri = 0; ri < rooms.length; ri++) {
                var other = rooms[ri];
                if (other.index === s0.room) continue;
                var line = wallLine(other, facing);
                var along = (line.axis === "z") ? s0.world[0] : s0.world[2];
                var across = (line.axis === "z") ? s0.world[2] : s0.world[0];
                if (Math.abs(across - line.at) > 0.05) continue;          // not that wall
                if (along < line.lo + s0.width * 0.5 + 0.3) continue;     // in the corner
                if (along > line.hi - s0.width * 0.5 - 0.3) continue;
                var atOther = (line.axis === "z") ? along - other.centre[0]
                                                  : along - other.centre[2];
                var clash2 = false, existing = other.doors[facing];
                for (var q2 = 0; q2 < existing.length; q2++)
                    if (Math.abs(existing[q2].at - atOther) < s0.width + 0.5) clash2 = true;
                if (clash2) continue;
                var mate = addOpening(other, facing, Math.round(atOther * 100) / 100,
                                      s0.width, s0.height);
                s0.joined = true;
                edges.push({ a: s0, b: mate, blocked: false, loop: true });
                loops++;
                return true;
            }
            return false;
        }

        // ---- the phone box, at the origin, looking down -Z ----------------
        var start = addRoom("Vorraum", [0, 0, 0], sizeOf("Vorraum", rng), 0);
        open.push(addOpening(start, "front", 0, DOOR_W, DOOR_H));

        var guard = 0;
        while (rooms.length < want && open.length && guard++ < want * 12) {
            // Oldest socket first, so the house grows outwards evenly rather
            // than boring one corridor into the distance. The dice choose
            // among the first few, which is enough to make two seeds differ.
            var pickIdx = open.length > 2 ? rng.int(0, Math.min(4, open.length - 1)) : 0;
            var socket = open.splice(pickIdx, 1)[0];
            var parent = rooms[socket.room];
            var depth = parent.depth + 1;
            var dockSide = OPPOSITE[socket.side];

            var placed = null;
            for (var attempt = 0; attempt < 5 && !placed; attempt++) {
                var type = rng.weighted(zoneTable(depth));
                var dim = sizeOf(type, rng);
                var span = (dockSide === "front" || dockSide === "back")
                    ? dim.width + 2 * dim.wall : dim.depth;
                var at = slot(span, socket.width, rng);
                var centre = centreFor(dim, dockSide, at, socket.world);
                var f = foot(centre, dim);
                var clash = false;
                for (var i = 0; i < rooms.length; i++) {
                    if (overlaps(f, rooms[i].foot, 0.02)) { clash = true; break; }
                }
                if (clash) { refused.overlap++; continue; }
                placed = { type: type, dim: dim, at: at, centre: centre };
            }

            if (!placed) {
                // Nothing fits: the opening stays a doorway into a wall. That
                // is honest - a door that cannot lead anywhere is boarded up,
                // not deleted, because the parent's wall already has the hole.
                refused.noSpace++;
                socket.blocked = true;
                edges.push({ a: socket, b: null, blocked: true });
                continue;
            }

            var child = addRoom(placed.type, placed.centre, placed.dim, depth);
            var back = addOpening(child, dockSide, placed.at, socket.width, socket.height);
            edges.push({ a: socket, b: back, blocked: false });

            // ...and the ways on. A dead end is a room that draws zero - but
            // only while there is something else left to grow from: if this
            // room is the last open end in the house, a dead end here would
            // stop the generator with three rooms and call it a floor.
            var T = TYPES[placed.type];
            var more = rng.int(T.doors[0], T.doors[1]);
            if (open.length === 0 && more === 0 && rooms.length < want) more = 1;

            // Draw from the sides that CAN take a door - the dock wall is out,
            // and one opening per wall for now. Picking blindly and skipping
            // the misses is the same code with fewer doors than it asked for.
            // A long wall may take more than one door - that is what makes a
            // hallway a hallway instead of a tube with an end. Each candidate
            // is a WALL SLOT, so a nine metre corridor offers two and a
            // cupboard offers none.
            var free = [];
            var sides = ["front", "back", "left", "right"];
            for (var si = 0; si < sides.length; si++) {
                var sd = sides[si];
                if (sd === dockSide) continue;
                var sp = (sd === "front" || sd === "back")
                    ? placed.dim.width + 2 * placed.dim.wall : placed.dim.depth;
                if (sp < DOOR_W + 0.9) continue;          // too narrow for a frame
                var slots = Math.max(1, Math.floor(sp / 3.6));
                for (var sl = 0; sl < slots; sl++) free.push(sd);
            }
            for (var k = 0; k < more && free.length; k++) {
                var pickSide = free.splice(rng.int(0, free.length - 1), 1)[0];
                var span2 = (pickSide === "front" || pickSide === "back")
                    ? placed.dim.width + 2 * placed.dim.wall : placed.dim.depth;
                var at2 = slot(span2, DOOR_W, rng);
                // ...but two doors may not be cut into each other.
                var clear = true;
                var already = child.doors[pickSide];
                for (var q = 0; q < already.length; q++)
                    if (Math.abs(already[q].at - at2) < DOOR_W + 0.6) clear = false;
                if (!clear) continue;
                var made = addOpening(child, pickSide, at2, DOOR_W, DOOR_H);
                if (!tryJoin(made)) open.push(made);
            }
        }

        // ---- stitching: two rooms that already share a wall ---------------
        // The growth makes a tree, and a tree is a house you can only ever
        // walk one way through. But rooms on a module end up standing back to
        // back constantly - R03's right wall IS R07's left wall - and a door
        // between those two is free: no geometry has to move, and the graph
        // gets the loop GDD §5.1 asks for.
        //
        // Not every shared wall gets one, or the floor would be a grid of
        // openings; the dice decide, which is why this runs off the same seed.
        var STITCH_CHANCE = 0.60, MIN_SHARE = 1.9;
        for (var ai = 0; ai < rooms.length; ai++) {
            for (var bi = ai + 1; bi < rooms.length; bi++) {
                var ra = rooms[ai], rb = rooms[bi];
                var pairs = [["right", "left"], ["left", "right"],
                             ["front", "back"], ["back", "front"]];
                for (var pi = 0; pi < pairs.length; pi++) {
                    var sa = pairs[pi][0], sb = pairs[pi][1];
                    var la = wallLine(ra, sa), lb = wallLine(rb, sb);
                    if (la.axis !== lb.axis) continue;
                    if (Math.abs(la.at - lb.at) > 0.05) continue;
                    var lo = Math.max(la.lo, lb.lo), hi = Math.min(la.hi, lb.hi);
                    if (hi - lo < MIN_SHARE) continue;
                    if (rng.next() > STITCH_CHANCE) continue;
                    var mid = Math.round(((lo + hi) * 0.5) / 0.3) * 0.3;
                    var atA = (la.axis === "z") ? mid - ra.centre[0] : mid - ra.centre[2];
                    var atB = (lb.axis === "z") ? mid - rb.centre[0] : mid - rb.centre[2];
                    var busy = false, i3, ea = ra.doors[sa], eb2 = rb.doors[sb];
                    for (i3 = 0; i3 < ea.length; i3++)
                        if (Math.abs(ea[i3].at - atA) < DOOR_W + 0.5) busy = true;
                    for (i3 = 0; i3 < eb2.length; i3++)
                        if (Math.abs(eb2[i3].at - atB) < DOOR_W + 0.5) busy = true;
                    if (busy) continue;
                    var oa = addOpening(ra, sa, Math.round(atA * 100) / 100, DOOR_W, DOOR_H);
                    var ob = addOpening(rb, sb, Math.round(atB * 100) / 100, DOOR_W, DOOR_H);
                    edges.push({ a: oa, b: ob, blocked: false, loop: true });
                    loops++;
                    break;                       // one door per pair of rooms
                }
            }
        }

        // Whatever is still open when the growth stops: a doorway with nothing
        // behind it yet. One last try at joining, then it is boarded up.
        for (var k2 = 0; k2 < open.length; k2++) {
            var left = open[k2];
            if (left.joined) continue;
            if (tryJoin(left)) continue;
            edges.push({ a: left, b: null, blocked: true, pending: !left.blocked });
        }

        return { seed: seed, rooms: rooms, edges: edges, loops: loops,
                 refused: refused };
    }

    // ----------------------------------------------------------------- build
    function build(p) {
        var lit = 0, leaves = 0;
        var tueren = INNEN.group("Tueren", 0, [0, 0, 0]);

        for (var i = 0; i < p.rooms.length; i++) {
            var r = p.rooms[i];
            var T = TYPES[r.type];
            var mats = MATS[T.mats];
            var g = INNEN.room(r.name, r.centre, r.dim, r.doors, mats);
            node.setStr(g.node, "node.tag", r.type);

            // Ceiling lights, spread along the room's length, the last one
            // always the weakest: a room whose far end is lit has nothing in
            // it worth being afraid of.
            var n = T.lights;
            for (var l = 0; l < n; l++) {
                var t = (n === 1) ? 0 : (l / (n - 1) - 0.5);
                var strength = 1.0 - 0.55 * (l / Math.max(1, n - 1));
                INNEN.lamp(r.name + ".Licht." + (l + 1), 0,
                    [r.centre[0], r.dim.height - 0.16, r.centre[2] + t * r.dim.depth * 0.7],
                    (6.0 + r.dim.width) * strength,
                    Math.max(3.0, r.dim.width) * 1.1,
                    [1.0, 0.90 - 0.04 * l, 0.74 - 0.06 * l],
                    [0.22, 0.05, 0.22]);
                lit++;
            }
        }

        // One leaf per doorway. A blocked opening gets a leaf that is locked:
        // the hole in the wall is real, and what is behind it is not.
        for (var e = 0; e < p.edges.length; e++) {
            var ed = p.edges[e];
            var s = ed.a;
            var yaw = (s.side === "front" || s.side === "back") ? 0 : 90;
            var hinge = (e % 2) ? 1 : -1;
            var locked = ed.blocked ? true : (e % 7 === 3);
            // ...and a door is "Tuer04", with the two rooms it joins in its
            // tag: "3>7", or "3>-1" when there is nothing behind it yet.
            var leaf = INNEN.door("Tuer" + pad2(e), tueren,
                       [s.world[0], 0, s.world[2]], yaw, s.width, s.height,
                       MATS[TYPES[p.rooms[s.room].type].mats].wall,
                       { hinge: hinge, openAngle: hinge > 0 ? -88 : 88,
                         locked: locked, speed: 140 });
            node.setStr(leaf, "node.tag",
                        s.room + ">" + (ed.b ? ed.b.room : -1));
            leaves++;
        }
        return { lights: lit, leaves: leaves };
    }

    editor.begin("INNEN: generated floor, seed " + SEED);
    var p = plan(SEED, WANT);
    var made = { lights: 0, leaves: 0 };
    if (!PLAN_ONLY) {
        made = build(p);
        if (WITH_PLAYER) INNEN.player("Spieler", [0, 0.95, 0.30], 0);

        // Das Gehaeuse: the rules of GDD §5.2, on a node of their own. It
        // finds the rooms and doors by the names above - "Raum03", "Tuer07" -
        // because a behaviour has no editor to enumerate the document with.
        if (has.script) {
            var haus = INNEN.group("Haus", 0, [0, 0, 0]);
            node.setStr(haus, "script",
                "innen_haus.js{rooms=" + p.rooms.length +
                ",doors=" + made.leaves + ",seed=" + SEED +
                ",warm=3,coldDistance=3,pingpong=3,anchors=0,player=Spieler" +
                ",puls=Puls,ankerNode=Anker,torch=Spieler.Lampe" +
                ",torchCone=26,torchRange=16}");

            // §5.4: the breathing, on a node of its own. It owns nothing but
            // the clock - which phase it is is written into ITS tag, and the
            // house above reads it. Two behaviours, one channel, the same one
            // the house already uses to lock a door.
            var puls = INNEN.group("Puls", 0, [0, 0, 0]);
            node.setStr(puls, "script",
                "innen_puls.js{rooms=" + p.rooms.length +
                ",seed=" + SEED + ",hellMin=240,hellMax=420" +
                ",dunkelMin=40,dunkelMax=90,flackerZeit=5" +
                ",ersteHell=95,player=Spieler,torch=Spieler.Lampe,haus=Haus}");

            // §5.3: the five real objects, and the behaviour that carries
            // them. They start in the phone box - room 0, where the player
            // steps out of his car's worth of belongings - laid out along its
            // floor so that two of them are never in the same place.
            made.anchors = 0;
            if (p.rooms.length) {
                var r0 = p.rooms[0];
                for (var ai = 0; ai < INNEN.ANKER.length; ai++) {
                    var off = (ai - (INNEN.ANKER.length - 1) * 0.5);
                    var ax = r0.centre[0] + off * Math.min(0.42, r0.dim.width * 0.16);
                    var az = r0.centre[2] + (ai % 2 ? 0.16 : -0.16);
                    var ay = 0.06 + INNEN.ANKER[ai].size[1] * 0.5;
                    if (INNEN.anker(ai, [ax, ay, az],
                                    MATS[TYPES[r0.type].mats].wall) >= 0)
                        made.anchors++;
                }
                var names = [];
                for (var an = 0; an < INNEN.ANKER.length; an++)
                    names.push("Anker." + INNEN.ANKER[an].key);
                var ankerNode = INNEN.group("Anker", 0, [0, 0, 0]);
                node.setStr(ankerNode, "script",
                    "innen_anker.js{rooms=" + p.rooms.length +
                    ",player=Spieler,haus=Haus,slots=4,ankerSlots=2" +
                    ",reach=1.8,items=" + names.join("|") + "}");
            }
        }
    }
    editor.commit();

    // The answer is the PLAN, not a report of it: the test reads room centres
    // and edges out of this and then asks the document whether it agrees.
    var out = {
        seed: SEED, rooms: [], edges: [], loops: p.loops,
        refused: p.refused, lights: made.lights, leaves: made.leaves,
        anchors: made.anchors || 0,
        nodes: editor.count(), player: WITH_PLAYER && !PLAN_ONLY
    };
    for (var a = 0; a < p.rooms.length; a++) {
        var rr = p.rooms[a];
        out.rooms.push({
            name: rr.name, type: rr.type, depth: rr.depth,
            centre: [rr.centre[0], rr.centre[2]],
            size: [rr.dim.width, rr.dim.depth, rr.dim.height, rr.dim.wall],
            foot: rr.foot,
            doors: (rr.doors.front.length + rr.doors.back.length +
                    rr.doors.left.length + rr.doors.right.length)
        });
    }
    for (var b = 0; b < p.edges.length; b++) {
        var eb = p.edges[b];
        out.edges.push({
            a: eb.a.room, aSide: eb.a.side,
            b: eb.b ? eb.b.room : -1,
            world: [eb.a.world[0], eb.a.world[2]],
            width: eb.a.width, height: eb.a.height,
            blocked: !!eb.blocked, loop: !!eb.loop,
            // "pending" is the honest word for most of them: the growth hit
            // its room budget with sockets still open. In the finished game
            // that is exactly the door the runtime generator grows a room
            // behind when the player opens it (GDD §5.1) - it is not a
            // failure, it is where the house has not happened yet.
            pending: !!eb.pending
        });
    }
    return JSON.stringify(out);
})();
