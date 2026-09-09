// INNEN — M0: the first three rooms, as DATA.
//
// Sent down the Jarvis bridge as one {"cmd":"eval"} (tools/daibridge.py js -f).
// Nothing here is compiled: the room generator of the game will emit exactly
// these calls, so this file is both the first playable slice and the shape the
// generator has to grow into.
//
// Metres, +Y up, floor of every room at y = 0. A room is a BOX with holes cut
// into named sides; every hole gets a DoorSocket, and a socket is what the
// next room docks onto. That is the whole contract — the generator will pick
// rooms and match sockets, it will not invent geometry.
//
//   Zelle    the phone box you step into      1.3 x 1.3 x 2.30, rusted sheet
//   Flur     your parents' hallway            1.7 x 9.0 x 2.55, woodchip + vinyl
//   Halle    the room that is too big          9.0 x 7.0 x 4.20, concrete
//
// They are placed by their sockets, in a line along -Z, so walking out of the
// box puts you in the hallway and the hallway ends in a room that cannot fit
// in the house it is wallpapered like.

(function () {
    var M = {
        floor:   "materials/pvc.daimat",
        wall:    "materials/raufaser.daimat",
        metal:   "materials/rost.daimat",
        ceiling: "materials/beton.daimat"
    };

    // ---- does this build know a property? asked on a scratch node ----------
    function supports(prop) {
        var probe = editor.add("__probe");
        node.setNum(probe, prop, 3);
        var known = node.getNum(probe, prop) === 3;
        editor.remove(probe);
        return known;
    }
    var hasBlockout = supports("blockout.kind");
    var hasCsg      = supports("csg.op");
    var hasSocket   = supports("door.width");
    var hasModifier = supports("modifier.0.type");

    // The same question for a TEXT property: setNum on a string field would
    // answer the fallback and prove nothing.
    function supports2(prop) {
        var probe = editor.add("__probe2");
        var known;
        if (prop === "script") {
            node.setStr(probe, prop, "x.js");
            known = node.getStr(probe, prop) === "x.js";
        } else {
            node.setNum(probe, prop, 3);
            known = node.getNum(probe, prop) === 3;
        }
        editor.remove(probe);
        return known;
    }

    var BOX = 1, CYL = 2, STAIRS = 3;      // dai_blockout_kind
    var SUBTRACT = 2;                       // dai_csg_op
    var MOD_BEVEL = 1, MOD_SUBDIV = 2, MOD_SOLIDIFY = 3, MOD_ARRAY = 4;

    var hasScript   = supports2("script");
    var hasCollider = supports2("collider.shape");
    var hasFreeze   = supports2("rigidbody.freeze");

    editor.begin("INNEN M0: Zelle, Flur, Halle");

    // Graphics only: no collider, no body. Every blockout and CSG node is one
    // of these, because a blockout node's COLLIDER is not its shape - the
    // document's half extent is still the default 1 m box, so a wall built out
    // of blockout children collides as a metre cube standing in the middle of
    // the room. That is what wedged the player in the phone box: the walk ran,
    // the velocity was right, and an invisible crate held him still.
    function noPhysics(n) {
        node.setNum(n, "rigidbody.enabled", 0);
        if (hasCollider) node.setNum(n, "collider.enabled", 0);
        return n;
    }

    // An invisible box that only collides. This is the ordinary way round in
    // every engine: the mesh is what you see, the collider is what you hit,
    // and for a wall with a hole in it they are not the same object.
    function collider(name, parent, pos, size) {
        var n = editor.add(name, parent);
        node.setPos(n, pos[0], pos[1], pos[2]);
        node.setVec(n, "transform.scale", size[0], size[1], size[2]);
        node.setNum(n, "rigidbody.motion", 0);      // static
        node.setNum(n, "renderer.enabled", 0);
        return n;
    }

    function place(name, parent, pos, size, material) {
        var n = editor.add(name, parent || 0);
        node.setPos(n, pos[0], pos[1], pos[2]);
        node.setVec(n, "transform.scale", size[0], size[1], size[2]);
        node.setNum(n, "rigidbody.motion", 0);       // architecture is static
        if (material) editor.setMaterial(n, material);
        return n;
    }

    function group(name, parent, pos) {
        var n = editor.add(name, parent || 0);
        node.setPos(n, pos[0], pos[1], pos[2]);
        node.setNum(n, "renderer.enabled", 0);       // a place, not a thing
        node.setNum(n, "rigidbody.enabled", 0);
        return n;
    }

    // A wall with zero or more openings. With CSG it is one subtraction node,
    // without it the same wall out of the pieces around the holes — the room
    // stands either way, which is what keeps the picture honest on an older
    // build.
    //
    //   axis "x": wall in the YZ plane (left/right), span is Z
    //   axis "z": wall in the XY plane (front/back),  span is X
    // The solid parts of a wall with openings in it, as invisible boxes: the
    // pieces left and right of each hole and the lintel above it. Exactly the
    // geometry the no-CSG fallback draws - which is the point: the picture and
    // the collision are the same wall, made twice on purpose.
    function wallColliders(name, parent, axis, at, span, height, thick, holes) {
        var horizontal = (axis === "z");
        var sorted = holes.slice().sort(function (a, b) { return a.at - b.at; });
        var cursor = -span * 0.5;
        for (var j = 0; j < sorted.length; j++) {
            var h = sorted[j];
            var l = h.at - h.width * 0.5, r = h.at + h.width * 0.5;
            if (l > cursor + 0.001) {
                var segAt = (cursor + l) * 0.5, segLen = l - cursor;
                collider(name + ".Hit" + j, parent,
                    horizontal ? [segAt, height * 0.5, at] : [at, height * 0.5, segAt],
                    horizontal ? [segLen, height, thick] : [thick, height, segLen]);
            }
            var lintel = height - h.height;
            if (lintel > 0.001)
                collider(name + ".HitLintel" + j, parent,
                    horizontal ? [h.at, h.height + lintel * 0.5, at]
                               : [at, h.height + lintel * 0.5, h.at],
                    horizontal ? [h.width, lintel, thick] : [thick, lintel, h.width]);
            cursor = r;
        }
        if (cursor < span * 0.5 - 0.001) {
            var tailAt = (cursor + span * 0.5) * 0.5, tailLen = span * 0.5 - cursor;
            collider(name + ".HitEnd", parent,
                horizontal ? [tailAt, height * 0.5, at] : [at, height * 0.5, tailAt],
                horizontal ? [tailLen, height, thick] : [thick, height, tailLen]);
        }
    }

    function wall(name, parent, axis, at, span, height, thick, holes, material) {
        var horizontal = (axis === "z");
        var pos = horizontal ? [0, height * 0.5, at] : [at, height * 0.5, 0];
        var size = horizontal ? [span, height, thick] : [thick, height, span];

        if (!holes || holes.length === 0)
            return place(name, parent, pos, size, material);

        if (hasBlockout && hasCsg) {
            var w = editor.add(name, parent);
            node.setPos(w, pos[0], pos[1], pos[2]);
            node.setNum(w, "csg.op", SUBTRACT);
            noPhysics(w);
            editor.setMaterial(w, material);

            var slab = editor.add(name + ".Slab", w);
            node.setNum(slab, "blockout.kind", BOX);
            node.setVec(slab, "blockout.size", size[0], size[1], size[2]);
            node.setPos(slab, 0, 0, 0);
            noPhysics(slab);

            for (var i = 0; i < holes.length; i++) {
                var h = holes[i];
                var cut = editor.add(name + ".Opening" + i, w);
                node.setNum(cut, "blockout.kind", BOX);
                var cs = horizontal ? [h.width, h.height, thick * 3]
                                    : [thick * 3, h.height, h.width];
                node.setVec(cut, "blockout.size", cs[0], cs[1], cs[2]);
                var cy = -(height - h.height) * 0.5;
                if (horizontal) node.setPos(cut, h.at, cy, 0);
                else            node.setPos(cut, 0, cy, h.at);
                noPhysics(cut);          // a hole must not collide. Obviously.
            }
            wallColliders(name, parent, axis, at, span, height, thick, holes);
            return w;
        }

        // No CSG: left piece, right piece, lintel, per hole (holes are sorted
        // and do not touch in M0, so one pass is enough).
        var w2 = group(name, parent, [0, 0, 0]);
        var cursor = -span * 0.5;
        for (var j = 0; j < holes.length; j++) {
            var hh = holes[j];
            var l = hh.at - hh.width * 0.5, r = hh.at + hh.width * 0.5;
            if (l > cursor) {
                var segAt = (cursor + l) * 0.5, segLen = l - cursor;
                place(name + ".Solid" + j, w2,
                      horizontal ? [segAt, height * 0.5, at] : [at, height * 0.5, segAt],
                      horizontal ? [segLen, height, thick] : [thick, height, segLen],
                      material);
            }
            var lintel = height - hh.height;
            if (lintel > 0.001)
                place(name + ".Lintel" + j, w2,
                      horizontal ? [hh.at, hh.height + lintel * 0.5, at]
                                 : [at, hh.height + lintel * 0.5, hh.at],
                      horizontal ? [hh.width, lintel, thick] : [thick, lintel, hh.width],
                      material);
            cursor = r;
        }
        if (cursor < span * 0.5) {
            var tailAt = (cursor + span * 0.5) * 0.5, tailLen = span * 0.5 - cursor;
            place(name + ".SolidEnd", w2,
                  horizontal ? [tailAt, height * 0.5, at] : [at, height * 0.5, tailAt],
                  horizontal ? [tailLen, height, thick] : [thick, height, tailLen],
                  material);
        }
        return w2;
    }

    // The anchor the generator docks the next room onto. It is a marker, never
    // a box standing in the doorway.
    function socket(name, parent, pos, normal, w, h) {
        var s = editor.add(name, parent);
        node.setPos(s, pos[0], pos[1], pos[2]);
        node.setNum(s, "rigidbody.enabled", 0);
        if (hasSocket) {
            node.setNum(s, "door.width", w);
            node.setNum(s, "door.height", h);
            node.setVec(s, "door.normal", normal[0], normal[1], normal[2]);
            node.setVec(s, "transform.scale", 0.05, 0.05, 0.05);
        } else {
            node.setVec(s, "transform.scale", w, 0.06, 0.06);
            editor.setMaterial(s, M.metal);
        }
        return s;
    }

    function lamp(name, parent, pos, intensity, range, color, size) {
        var l = editor.add(name, parent);
        node.setPos(l, pos[0], pos[1], pos[2]);
        node.setNum(l, "light.mode", 1);
        node.setNum(l, "light.intensity", intensity);
        node.setNum(l, "light.range", range);
        node.setVec(l, "light.color", color[0], color[1], color[2]);
        node.setNum(l, "rigidbody.enabled", 0);
        node.setVec(l, "transform.scale", size[0], size[1], size[2]);
        return l;
    }

    // A room: floor, ceiling, four walls, the openings named per side.
    // `doors` is { front: [{at,width,height}], back: [...], left: [...], right: [...] }
    // front is -Z, back is +Z, left is -X, right is +X.
    function room(name, at, dim, doors, mats) {
        var t = dim.wall, W = dim.width, D = dim.depth, H = dim.height;
        var g = group(name, 0, at);
        var d = doors || {};

        place(name + ".Floor", g, [0, -t * 0.5, 0], [W + 2 * t, t, D + 2 * t], mats.floor);
        place(name + ".Ceiling", g, [0, H + t * 0.5, 0], [W + 2 * t, t, D + 2 * t], mats.ceiling);

        wall(name + ".Wall.Front", g, "z", -(D * 0.5 + t * 0.5), W + 2 * t, H, t, d.front, mats.wall);
        wall(name + ".Wall.Back",  g, "z",  (D * 0.5 + t * 0.5), W + 2 * t, H, t, d.back,  mats.wall);
        wall(name + ".Wall.Left",  g, "x", -(W * 0.5 + t * 0.5), D,        H, t, d.left,  mats.wall);
        wall(name + ".Wall.Right", g, "x",  (W * 0.5 + t * 0.5), D,        H, t, d.right, mats.wall);

        var sides = [["front", [0, 0, -1]], ["back", [0, 0, 1]],
                     ["left", [-1, 0, 0]], ["right", [1, 0, 0]]];
        for (var s = 0; s < sides.length; s++) {
            var key = sides[s][0], n = sides[s][1], list = d[key];
            if (!list) continue;
            for (var i = 0; i < list.length; i++) {
                var hole = list[i];
                var p = (key === "front" || key === "back")
                    ? [hole.at, hole.height * 0.5, n[2] * (D * 0.5 + t * 0.5)]
                    : [n[0] * (W * 0.5 + t * 0.5), hole.height * 0.5, hole.at];
                socket(name + ".Door." + key + i, g, p, n, hole.width, hole.height);
            }
        }
        return g;
    }

    // ------------------------------------------------------------------ 1/3
    // Die Zelle. You walk in to make a call; the door shuts behind you.
    var DOOR = { width: 0.95, height: 2.02 };
    var zelle = room("Zelle", [0, 0, 0],
        { width: 1.30, depth: 1.30, height: 2.30, wall: 0.12 },
        { front: [{ at: 0, width: DOOR.width, height: DOOR.height }] },
        { floor: M.metal, wall: M.metal, ceiling: M.metal });

    // The phone: body, handset on the cradle, cable. Kitbash out of blockout
    // primitives with modifiers, which is how detail is made here — never by
    // pushing vertices.
    var phone = group("Zelle.Telefon", zelle, [0, 1.15, 0.52]);
    var body = editor.add("Zelle.Telefon.Korpus", phone);
    node.setPos(body, 0, 0, 0);
    node.setNum(body, "rigidbody.motion", 0);
    if (hasBlockout) {
        node.setNum(body, "blockout.kind", BOX);
        node.setVec(body, "blockout.size", 0.26, 0.34, 0.14);
        if (hasModifier) {
            node.setNum(body, "modifier.0.type", MOD_BEVEL);
            node.setNum(body, "modifier.0.width", 0.012);
            node.setNum(body, "modifier.0.segments", 2);
            node.setNum(body, "modifier.count", 1);
        }
    } else {
        node.setVec(body, "transform.scale", 0.26, 0.34, 0.14);
    }
    noPhysics(body);
    editor.setMaterial(body, M.metal);

    var horn = editor.add("Zelle.Telefon.Hoerer", phone);
    node.setPos(horn, 0, 0.20, -0.10);
    node.setNum(horn, "rigidbody.motion", 0);
    if (hasBlockout) {
        node.setNum(horn, "blockout.kind", CYL);
        node.setVec(horn, "blockout.size", 0.05, 0.22, 0.05);
        node.setNum(horn, "blockout.segments", 12);
    } else {
        node.setVec(horn, "transform.scale", 0.05, 0.22, 0.05);
    }
    node.setVec(horn, "transform.rotation", 0, 0, 90);
    noPhysics(horn);
    editor.setMaterial(horn, M.metal);

    var shelf = place("Zelle.Telefon.Ablage", phone, [0, -0.22, -0.09],
                      [0.34, 0.03, 0.20], M.metal);

    lamp("Zelle.Licht", zelle, [0, 2.14, 0], 6.0, 3.2, [1.0, 0.86, 0.62], [0.10, 0.04, 0.10]);

    // ------------------------------------------------------------------ 2/3
    // Der Flur. Your parents' hallway, remembered a little too narrow.
    // Docked in front of the box: the box's front socket is at z = -0.71, so
    // the hallway's back wall lands there.
    var flurDim = { width: 1.70, depth: 9.00, height: 2.55, wall: 0.15 };
    var flurZ = -(0.65 + 0.06) - (flurDim.depth * 0.5 + flurDim.wall * 0.5);
    var flur = room("Flur", [0, 0, flurZ], flurDim,
        {
            back:  [{ at: 0, width: DOOR.width, height: DOOR.height }],
            front: [{ at: 0, width: 1.10, height: 2.10 }],
            left:  [{ at: 2.6, width: 0.85, height: 1.98 },
                    { at: -1.9, width: 0.85, height: 1.98 }],
            right: [{ at: 0.4, width: 0.85, height: 1.98 }]
        },
        { floor: M.floor, wall: M.wall, ceiling: M.wall });

    // Three ceiling lights, the middle one dimmer: the far end of a hallway is
    // where the eye goes, so it is the part that is not lit.
    lamp("Flur.Licht.1", flur, [0, 2.38,  3.3], 9.0, 5.0, [1.0, 0.90, 0.72], [0.22, 0.05, 0.22]);
    lamp("Flur.Licht.2", flur, [0, 2.38,  0.0], 4.5, 4.0, [1.0, 0.88, 0.70], [0.22, 0.05, 0.22]);
    lamp("Flur.Licht.3", flur, [0, 2.38, -3.3], 2.0, 3.2, [0.95, 0.85, 0.70], [0.22, 0.05, 0.22]);

    // Skirting board along both sides — one box plus an array modifier where
    // the stack exists, one long box where it does not.
    var sockelL = editor.add("Flur.Sockelleiste.Links", flur);
    node.setPos(sockelL, -(flurDim.width * 0.5 - 0.01), 0.06, 0);
    node.setNum(sockelL, "rigidbody.motion", 0);
    if (hasBlockout) {
        node.setNum(sockelL, "blockout.kind", BOX);
        node.setVec(sockelL, "blockout.size", 0.02, 0.12, flurDim.depth);
    } else {
        node.setVec(sockelL, "transform.scale", 0.02, 0.12, flurDim.depth);
    }
    noPhysics(sockelL);
    editor.setMaterial(sockelL, M.metal);

    var sockelR = editor.add("Flur.Sockelleiste.Rechts", flur);
    node.setPos(sockelR, (flurDim.width * 0.5 - 0.01), 0.06, 0);
    node.setNum(sockelR, "rigidbody.motion", 0);
    if (hasBlockout) {
        node.setNum(sockelR, "blockout.kind", BOX);
        node.setVec(sockelR, "blockout.size", 0.02, 0.12, flurDim.depth);
    } else {
        node.setVec(sockelR, "transform.scale", 0.02, 0.12, flurDim.depth);
    }
    noPhysics(sockelR);
    editor.setMaterial(sockelR, M.metal);

    // ------------------------------------------------------------------ 3/3
    // Die Halle. Same wallpaper, four metres of ceiling: the first room that
    // says out loud that this is not the house.
    var halleDim = { width: 9.00, depth: 7.00, height: 4.20, wall: 0.20 };
    var halleZ = flurZ - (flurDim.depth * 0.5 + flurDim.wall * 0.5)
                       - (halleDim.depth * 0.5 + halleDim.wall * 0.5);
    var halle = room("Halle", [0, 0, halleZ], halleDim,
        {
            back:  [{ at: 0, width: 1.10, height: 2.10 }],
            left:  [{ at: -1.0, width: 1.10, height: 2.10 }],
            front: [{ at: 2.8, width: 1.10, height: 2.10 }]
        },
        { floor: M.floor, wall: M.wall, ceiling: M.ceiling });

    lamp("Halle.Licht.1", halle, [-2.6, 3.95, 1.8], 16.0, 9.0, [0.98, 0.95, 0.86], [0.5, 0.06, 0.5]);
    lamp("Halle.Licht.2", halle, [ 2.6, 3.95, -1.4], 12.0, 8.0, [0.98, 0.95, 0.86], [0.5, 0.06, 0.5]);

    // A staircase that goes up into the ceiling. It is not a way out; it is
    // the room being wrong in a way the player can walk on.
    var treppe = editor.add("Halle.Treppe", halle);
    node.setPos(treppe, -3.2, 0, -2.2);
    node.setNum(treppe, "rigidbody.motion", 0);
    if (hasBlockout) {
        node.setNum(treppe, "blockout.kind", STAIRS);
        node.setVec(treppe, "blockout.size", 1.20, 2.60, 3.20);
        node.setNum(treppe, "blockout.steps", 13);
    } else {
        node.setVec(treppe, "transform.scale", 1.20, 2.60, 3.20);
    }
    noPhysics(treppe);
    editor.setMaterial(treppe, M.ceiling);

    // ------------------------------------------------------------ der Spieler
    // A capsule with the behaviour on it, a camera in its head and a spot
    // light in its hand. All three are placed HERE, in the level script,
    // because a player that only exists when somebody drags three nodes
    // together by hand is a player no test can walk.

    var player = editor.add("Spieler", 0);
    // Standing in the box, a step back from the door, on the floor: the
    // transform is at the FEET and the capsule is lifted by collider.center,
    // which is what makes "put the player at floor level" mean what it says.
    // The capsule: transform.scale x is the RADIUS factor and y the half
    // height factor of the shape's own half extent (0.5), so 0.55 x 1.30 is a
    // body 0.55 m across and 2*(0.65+0.275) = 1.85 m tall. It was 1.75 once,
    // which made a capsule exactly 2.30 m tall - the inner height of the phone
    // box - and the player stood there wedged between the floor and the
    // ceiling with the walk running and nothing moving.
    node.setPos(player, 0.0, 0.95, 0.30);
    node.setVec(player, "transform.scale", 0.55, 1.30, 0.55);
    node.setNum(player, "rigidbody.motion", 2);          // DAI_DYNAMIC
    node.setNum(player, "rigidbody.friction", 0.2);
    node.setNum(player, "renderer.enabled", 0);          // the eye is inside it
    if (hasCollider) node.setNum(player, "collider.shape", 2);   // DAI_SHAPE_CAPSULE
    if (hasFreeze)   node.setNum(player, "rigidbody.freeze", 40); // UPRIGHT: rotX|rotZ
    node.setStr(player, "node.tag", "Player");

    var eye = editor.add("Spieler.Kamera", player);
    node.setPos(eye, 0, 0.62, 0);          // above the capsule's CENTRE
    node.setNum(eye, "camera.mode", 1);
    node.setNum(eye, "camera.fov", 75);
    node.setNum(eye, "renderer.enabled", 0);
    node.setNum(eye, "rigidbody.enabled", 0);

    var torch = editor.add("Spieler.Lampe", player);
    node.setPos(torch, 0.18, 0.48, 0);
    node.setNum(torch, "light.mode", 2);                 // spot
    node.setNum(torch, "light.cone", 26);
    node.setNum(torch, "light.range", 16);
    node.setNum(torch, "light.intensity", 9);
    node.setVec(torch, "light.color", 1.0, 0.94, 0.82);
    node.setNum(torch, "renderer.enabled", 0);
    node.setNum(torch, "rigidbody.enabled", 0);
    node.setVec(torch, "transform.scale", 0.08, 0.08, 0.08);

    // The behaviour and its inspector values, in the form the document stores
    // them: "file.js{key=value,...}", ';' separated for a stack of scripts.
    // The two node references are names, which is what a node field is at play
    // time.
    if (hasScript)
        node.setStr(player, "script",
            "innen_player.js{eye=Spieler.Kamera,torch=Spieler.Lampe," +
            "walkSpeed=2.2,eyeOffset=0.62,torchOn=true}");

    editor.commit();

    return JSON.stringify({
        rooms: [zelle, flur, halle],
        nodes: editor.count(),
        csg: hasBlockout && hasCsg,
        socket: hasSocket,
        modifier: hasModifier,
        player: player,
        script: hasScript,
        collider: hasCollider,
        z: { zelle: 0, flur: flurZ, halle: halleZ }
    });
})();
