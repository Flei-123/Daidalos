// INNEN - the parts a room is made of. No room in here, only how to build one.
//
// Sent down the Jarvis bridge IN FRONT of a level script:
//
//     tools/daibridge.py js -f examples/scripts/innen_lib.js \
//                           -f examples/scripts/innen_m0.js
//
// The two files are concatenated into ONE eval, so this file leaves exactly
// one name behind - `INNEN` - and the level script uses it.
//
// WHY IT EXISTS: `innen_m0.js` is the first three rooms by hand and
// `innen_gen.js` grows a whole floor by itself, and both have to build the
// same kind of room. Two copies of `wall()` is two definitions of what a
// doorway IS, and the day they disagree the generator's rooms stop docking
// onto the hand built ones - in a way no test would name, because both files
// would still pass their own.
//
// Metres, +Y up, floor of every room at y = 0. A room is a BOX with holes cut
// into named sides; every hole gets a DoorSocket, and a socket is what the
// next room docks onto. front is -Z, back is +Z, left is -X, right is +X.

var INNEN = (function () {

    var M = {
        floor:   "materials/pvc.daimat",
        wall:    "materials/raufaser.daimat",
        metal:   "materials/rost.daimat",
        ceiling: "materials/beton.daimat"
    };

    var BOX = 1, CYL = 2, STAIRS = 3;       // dai_blockout_kind
    var SUBTRACT = 2;                       // dai_csg_op
    var MOD_BEVEL = 1, MOD_SUBDIV = 2, MOD_SOLIDIFY = 3, MOD_ARRAY = 4;

    // ---- does this build know a property? asked on a scratch node ----------
    function supports(prop) {
        var probe = editor.add("__probe");
        node.setNum(probe, prop, 3);
        var known = node.getNum(probe, prop) === 3;
        editor.remove(probe);
        return known;
    }

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

    var has = {
        blockout: supports("blockout.kind"),
        csg:      supports("csg.op"),
        socket:   supports("door.width"),
        modifier: supports("modifier.0.type"),
        script:   supports2("script"),
        collider: supports2("collider.shape"),
        freeze:   supports2("rigidbody.freeze")
    };

    // Graphics only: no collider, no body. Every blockout and CSG node is one
    // of these, because a blockout node's COLLIDER is not its shape - the
    // document's half extent is still the default 1 m box, so a wall built out
    // of blockout children collides as a metre cube standing in the middle of
    // the room. That is what wedged the player in the phone box: the walk ran,
    // the velocity was right, and an invisible crate held him still.
    function noPhysics(n) {
        node.setNum(n, "rigidbody.enabled", 0);
        if (has.collider) node.setNum(n, "collider.enabled", 0);
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

    // A wall with zero or more openings. With CSG it is one subtraction node,
    // without it the same wall out of the pieces around the holes - the room
    // stands either way, which is what keeps the picture honest on an older
    // build.
    //
    //   axis "x": wall in the YZ plane (left/right), span is Z
    //   axis "z": wall in the XY plane (front/back),  span is X
    function wall(name, parent, axis, at, span, height, thick, holes, material) {
        var horizontal = (axis === "z");
        var pos = horizontal ? [0, height * 0.5, at] : [at, height * 0.5, 0];
        var size = horizontal ? [span, height, thick] : [thick, height, span];

        if (!holes || holes.length === 0)
            return place(name, parent, pos, size, material);

        if (has.blockout && has.csg) {
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
        // and do not touch, so one pass is enough).
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
        if (has.socket) {
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

    var SIDES = [["front", [0, 0, -1]], ["back", [0, 0, 1]],
                 ["left", [-1, 0, 0]], ["right", [1, 0, 0]]];

    // A room: floor, ceiling, four walls, the openings named per side.
    // `doors` is { front: [{at,width,height}], back: [...], left: [...], right: [...] }
    //
    // Returns the group AND the sockets it made, in world coordinates - the
    // generator needs to know where the next room may dock, and asking the
    // document again for something this function just computed is how two
    // ideas of the same doorway start to drift apart.
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

        var made = [];
        for (var s = 0; s < SIDES.length; s++) {
            var key = SIDES[s][0], n = SIDES[s][1], list = d[key];
            if (!list) continue;
            for (var i = 0; i < list.length; i++) {
                var hole = list[i];
                var p = (key === "front" || key === "back")
                    ? [hole.at, hole.height * 0.5, n[2] * (D * 0.5 + t * 0.5)]
                    : [n[0] * (W * 0.5 + t * 0.5), hole.height * 0.5, hole.at];
                var sn = name + ".Door." + key + i;
                socket(sn, g, p, n, hole.width, hole.height);
                made.push({
                    name: sn, side: key, normal: n,
                    local: p,
                    world: [at[0] + p[0], at[1] + p[1], at[2] + p[2]],
                    width: hole.width, height: hole.height
                });
            }
        }
        return { node: g, name: name, at: at, dim: dim, sockets: made };
    }

    // A door LEAF: one box the size of the opening, with a collider, standing
    // in the doorway, carrying innen_door.js. See docs/INNEN_M0.md - the leaf
    // is kinematic and its handle is not a child, both for reasons that cost
    // an evening each.
    var LEAF_T = 0.045;                       // 45 mm of door

    function door(name, parent, pos, yaw, w, h, material, opts) {
        var o = opts || {};
        var leafW = w - 0.02, leafH = h - 0.02;   // a door is not its frame
        var leaf = editor.add(name, parent);
        node.setPos(leaf, pos[0], 0.01 + leafH * 0.5, pos[2]);
        node.setVec(leaf, "transform.scale", leafW, leafH, LEAF_T);
        node.setVec(leaf, "transform.rotation", 0, yaw, 0);
        // KINEMATIC: it is moved by a script and it stops a player. A static
        // body that teleports is a lie to the solver, a dynamic one falls over.
        node.setNum(leaf, "rigidbody.motion", 1);
        editor.setMaterial(leaf, material);

        // The handle, on the side away from the hinge. No physics and no
        // parenting - the door script carries it, which is the only way it
        // keeps up with a body that moves.
        var hinge = (o.hinge === undefined) ? 1 : o.hinge;
        var handleName = "";
        if (has.blockout) {
            handleName = name + ".Klinke";
            var kl = editor.add(handleName, parent);
            var hxLocal = -hinge * (leafW * 0.5 - 0.07);
            var c = Math.cos(yaw * Math.PI / 180), sn = Math.sin(yaw * Math.PI / 180);
            node.setPos(kl, pos[0] + hxLocal * c,
                            0.01 + leafH * 0.5 - 0.06,
                            pos[2] - hxLocal * sn);
            node.setNum(kl, "blockout.kind", CYL);
            node.setVec(kl, "blockout.size", 0.03, 0.13, 0.03);
            node.setNum(kl, "blockout.segments", 8);
            node.setVec(kl, "transform.rotation", 0, yaw, 90);
            noPhysics(kl);
            editor.setMaterial(kl, M.metal);
        }

        if (has.script) {
            var f = [];
            f.push("width=" + leafW);
            f.push("hinge=" + hinge);
            f.push("yaw=" + yaw);
            f.push("openAngle=" + (o.openAngle === undefined ? -85 : o.openAngle));
            f.push("speed=" + (o.speed === undefined ? 150 : o.speed));
            f.push("startOpen=" + (o.startOpen ? "true" : "false"));
            f.push("locked=" + (o.locked ? "true" : "false"));
            f.push("slamBehind=" + (o.slamBehind ? "true" : "false"));
            f.push("lockAfterSlam=" + (o.lockAfterSlam ? "true" : "false"));
            f.push("autoClose=" + (o.autoClose === undefined ? 0 : o.autoClose));
            f.push("player=Spieler");
            if (handleName) f.push("handle=" + handleName);
            node.setStr(leaf, "script", "innen_door.js{" + f.join(",") + "}");
        }
        return leaf;
    }

    // The player: a capsule with the behaviour on it, a camera in its head and
    // a spot light in its hand, all three placed by whoever builds the level,
    // because a player that only exists when somebody drags three nodes
    // together by hand is a player no test can walk.
    // The five things out of your car - GDD §5.3. They are the only objects
    // in the house that are REAL: not built out of somebody's memory, so the
    // house cannot overwrite them, and the room one lies in stops changing.
    //
    // They are plain boxes at the size the thing actually is. A warning
    // triangle is 0.43 m across and a petrol receipt is a slip of paper, and
    // the difference is the point: you can see across a room which one you
    // left there. The colour is the one Rot the art direction allows (§8) for
    // the triangle, and washed out 70s plastic for the rest.
    //
    // They are graphics only. An anchor is picked up by standing near it and
    // pressing E - innen_anker.js measures the distance - and a dynamic body
    // would spend the game rolling down the stair ramp in the hall.
    var ANKER = [
        { key: "Warndreieck",  size: [0.43, 0.40, 0.04] },
        { key: "ErsteHilfe",   size: [0.26, 0.18, 0.11] },
        { key: "Eiskratzer",   size: [0.12, 0.02, 0.24] },
        { key: "Tankquittung", size: [0.08, 0.001, 0.14] },
        { key: "Foto",         size: [0.09, 0.002, 0.13] }
    ];

    function anker(index, pos, material) {
        var a = ANKER[index];
        if (!a) return -1;
        var n = editor.add("Anker." + a.key, 0);
        node.setPos(n, pos[0], pos[1], pos[2]);
        node.setVec(n, "transform.scale", a.size[0], a.size[1], a.size[2]);
        node.setNum(n, "rigidbody.enabled", 0);     // picked up, not pushed
        node.setStr(n, "node.tag", "Anker");
        if (material) editor.setMaterial(n, material);
        return n;
    }

    function player(name, pos, yaw) {
        var p = editor.add(name || "Spieler", 0);
        // The transform is at the FEET and the capsule is lifted by
        // collider.center. transform.scale x is the RADIUS factor and y the
        // half height factor of the shape's own half extent (0.5), so
        // 0.55 x 1.30 is a body 0.55 m across and 1.85 m tall. It was 1.75
        // once, which made a capsule exactly 2.30 m tall - the inner height of
        // the phone box - and the player stood wedged between floor and
        // ceiling with the walk running and nothing moving.
        node.setPos(p, pos[0], pos[1], pos[2]);
        node.setVec(p, "transform.scale", 0.55, 1.30, 0.55);
        node.setNum(p, "rigidbody.motion", 2);              // DAI_DYNAMIC
        node.setNum(p, "rigidbody.friction", 0.2);
        node.setNum(p, "renderer.enabled", 0);              // the eye is inside it
        if (has.collider) node.setNum(p, "collider.shape", 2);    // DAI_SHAPE_CAPSULE
        if (has.freeze)   node.setNum(p, "rigidbody.freeze", 40); // UPRIGHT: rotX|rotZ
        node.setStr(p, "node.tag", "Player");

        var eye = editor.add("Spieler.Kamera", p);
        node.setPos(eye, 0, 0.62, 0);          // above the capsule's CENTRE
        node.setNum(eye, "camera.mode", 1);
        node.setNum(eye, "camera.fov", 75);
        node.setNum(eye, "renderer.enabled", 0);
        node.setNum(eye, "rigidbody.enabled", 0);

        var torch = editor.add("Spieler.Lampe", p);
        node.setPos(torch, 0.18, 0.48, 0);
        node.setNum(torch, "light.mode", 2);                // spot
        node.setNum(torch, "light.cone", 26);
        node.setNum(torch, "light.range", 16);
        node.setNum(torch, "light.intensity", 9);
        node.setVec(torch, "light.color", 1.0, 0.94, 0.82);
        node.setNum(torch, "renderer.enabled", 0);
        node.setNum(torch, "rigidbody.enabled", 0);
        node.setVec(torch, "transform.scale", 0.08, 0.08, 0.08);

        // The behaviour and its inspector values, in the form the document
        // stores them: "file.js{key=value,...}", ';' separated for a stack of
        // scripts. The two node references are names, which is what a node
        // field is at play time.
        if (has.script)
            node.setStr(p, "script",
                "innen_player.js{eye=Spieler.Kamera,torch=Spieler.Lampe," +
                "walkSpeed=2.2,eyeOffset=0.62,torchOn=true,startYaw=" +
                (yaw || 0) + "}");
        return p;
    }

    return {
        M: M, has: has,
        BOX: BOX, CYL: CYL, STAIRS: STAIRS, SUBTRACT: SUBTRACT,
        MOD_BEVEL: MOD_BEVEL, MOD_SUBDIV: MOD_SUBDIV,
        MOD_SOLIDIFY: MOD_SOLIDIFY, MOD_ARRAY: MOD_ARRAY,
        SIDES: SIDES,
        noPhysics: noPhysics, collider: collider, place: place, group: group,
        wall: wall, wallColliders: wallColliders, socket: socket, lamp: lamp,
        room: room, door: door, player: player,
        anker: anker, ANKER: ANKER
    };
})();
