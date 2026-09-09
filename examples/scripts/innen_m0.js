// INNEN — M0: the first three rooms, as DATA.
//
// Sent down the Jarvis bridge behind examples/scripts/innen_lib.js, which has
// the parts a room is made of:
//
//     tools/daibridge.py js -f examples/scripts/innen_lib.js \
//                           -f examples/scripts/innen_m0.js
//
// Nothing here is compiled: the room generator (examples/scripts/innen_gen.js)
// emits exactly these calls, so this file is both the first playable slice and
// the shape the generator had to grow into.
//
//   Zelle    the phone box you step into      1.3 x 1.3 x 2.30, rusted sheet
//   Flur     your parents' hallway            1.7 x 9.0 x 2.55, woodchip + vinyl
//   Halle    the room that is too big         9.0 x 7.0 x 4.20, concrete
//
// They are placed by their sockets, in a line along -Z, so walking out of the
// box puts you in the hallway and the hallway ends in a room that cannot fit
// in the house it is wallpapered like.

(function () {
    if (typeof INNEN === "undefined")
        throw new Error("innen_m0.js needs innen_lib.js in front of it");

    var M = INNEN.M, has = INNEN.has;
    var BOX = INNEN.BOX, CYL = INNEN.CYL, STAIRS = INNEN.STAIRS;

    editor.begin("INNEN M0: Zelle, Flur, Halle");

    // ------------------------------------------------------------------ 1/3
    // Die Zelle. You walk in to make a call; the door shuts behind you.
    var DOOR = { width: 0.95, height: 2.02 };
    var zelleR = INNEN.room("Zelle", [0, 0, 0],
        { width: 1.30, depth: 1.30, height: 2.30, wall: 0.12 },
        { front: [{ at: 0, width: DOOR.width, height: DOOR.height }] },
        { floor: M.metal, wall: M.metal, ceiling: M.metal });
    var zelle = zelleR.node;

    // The phone: body, handset on the cradle, a shelf. Kitbash out of blockout
    // primitives with modifiers, which is how detail is made here — never by
    // pushing vertices.
    var phone = INNEN.group("Zelle.Telefon", zelle, [0, 1.15, 0.52]);
    var body = editor.add("Zelle.Telefon.Korpus", phone);
    node.setPos(body, 0, 0, 0);
    node.setNum(body, "rigidbody.motion", 0);
    if (has.blockout) {
        node.setNum(body, "blockout.kind", BOX);
        node.setVec(body, "blockout.size", 0.26, 0.34, 0.14);
        if (has.modifier) {
            node.setNum(body, "modifier.0.type", INNEN.MOD_BEVEL);
            node.setNum(body, "modifier.0.width", 0.012);
            node.setNum(body, "modifier.0.segments", 2);
            node.setNum(body, "modifier.count", 1);
        }
    } else {
        node.setVec(body, "transform.scale", 0.26, 0.34, 0.14);
    }
    INNEN.noPhysics(body);
    editor.setMaterial(body, M.metal);

    var horn = editor.add("Zelle.Telefon.Hoerer", phone);
    node.setPos(horn, 0, 0.20, -0.10);
    node.setNum(horn, "rigidbody.motion", 0);
    if (has.blockout) {
        node.setNum(horn, "blockout.kind", CYL);
        node.setVec(horn, "blockout.size", 0.05, 0.22, 0.05);
        node.setNum(horn, "blockout.segments", 12);
    } else {
        node.setVec(horn, "transform.scale", 0.05, 0.22, 0.05);
    }
    node.setVec(horn, "transform.rotation", 0, 0, 90);
    INNEN.noPhysics(horn);
    editor.setMaterial(horn, M.metal);

    INNEN.place("Zelle.Telefon.Ablage", phone, [0, -0.22, -0.09],
                [0.34, 0.03, 0.20], M.metal);

    INNEN.lamp("Zelle.Licht", zelle, [0, 2.14, 0], 6.0, 3.2, [1.0, 0.86, 0.62],
               [0.10, 0.04, 0.10]);

    // ------------------------------------------------------------------ 2/3
    // Der Flur. Your parents' hallway, remembered a little too narrow.
    // Docked in front of the box: the box's front socket is at z = -0.71, so
    // the hallway's back wall lands there.
    var flurDim = { width: 1.70, depth: 9.00, height: 2.55, wall: 0.15 };
    var flurZ = -(0.65 + 0.06) - (flurDim.depth * 0.5 + flurDim.wall * 0.5);
    var flurR = INNEN.room("Flur", [0, 0, flurZ], flurDim,
        {
            back:  [{ at: 0, width: DOOR.width, height: DOOR.height }],
            front: [{ at: 0, width: 1.10, height: 2.10 }],
            left:  [{ at: 2.6, width: 0.85, height: 1.98 },
                    { at: -1.9, width: 0.85, height: 1.98 }],
            right: [{ at: 0.4, width: 0.85, height: 1.98 }]
        },
        { floor: M.floor, wall: M.wall, ceiling: M.wall });
    var flur = flurR.node;

    // Three ceiling lights, the far one nearly out: the end of a hallway is
    // where the eye goes, so it is the part that is not lit.
    INNEN.lamp("Flur.Licht.1", flur, [0, 2.38,  3.3], 9.0, 5.0, [1.0, 0.90, 0.72], [0.22, 0.05, 0.22]);
    INNEN.lamp("Flur.Licht.2", flur, [0, 2.38,  0.0], 4.5, 4.0, [1.0, 0.88, 0.70], [0.22, 0.05, 0.22]);
    INNEN.lamp("Flur.Licht.3", flur, [0, 2.38, -3.3], 2.0, 3.2, [0.95, 0.85, 0.70], [0.22, 0.05, 0.22]);

    // Skirting board along both sides.
    var sides = [["Links", -1], ["Rechts", 1]];
    for (var si = 0; si < sides.length; si++) {
        var sk = editor.add("Flur.Sockelleiste." + sides[si][0], flur);
        node.setPos(sk, sides[si][1] * (flurDim.width * 0.5 - 0.01), 0.06, 0);
        node.setNum(sk, "rigidbody.motion", 0);
        if (has.blockout) {
            node.setNum(sk, "blockout.kind", BOX);
            node.setVec(sk, "blockout.size", 0.02, 0.12, flurDim.depth);
        } else {
            node.setVec(sk, "transform.scale", 0.02, 0.12, flurDim.depth);
        }
        INNEN.noPhysics(sk);
        editor.setMaterial(sk, M.metal);
    }

    // ------------------------------------------------------------------ 3/3
    // Die Halle. Same wallpaper, four metres of ceiling: the first room that
    // says out loud that this is not the house.
    var halleDim = { width: 9.00, depth: 7.00, height: 4.20, wall: 0.20 };
    var halleZ = flurZ - (flurDim.depth * 0.5 + flurDim.wall * 0.5)
                       - (halleDim.depth * 0.5 + halleDim.wall * 0.5);
    var halleR = INNEN.room("Halle", [0, 0, halleZ], halleDim,
        {
            back:  [{ at: 0, width: 1.10, height: 2.10 }],
            left:  [{ at: -1.0, width: 1.10, height: 2.10 }],
            front: [{ at: 2.8, width: 1.10, height: 2.10 }]
        },
        { floor: M.floor, wall: M.wall, ceiling: M.ceiling });
    var halle = halleR.node;

    INNEN.lamp("Halle.Licht.1", halle, [-2.6, 3.95, 1.8], 16.0, 9.0, [0.98, 0.95, 0.86], [0.5, 0.06, 0.5]);
    INNEN.lamp("Halle.Licht.2", halle, [ 2.6, 3.95, -1.4], 12.0, 8.0, [0.98, 0.95, 0.86], [0.5, 0.06, 0.5]);

    // A staircase that goes up INTO the ceiling. It is not a way out; it is
    // the room being wrong in a way the player can walk on - and walking on it
    // is the point, so it has treads you can climb and not just a picture of
    // them.
    //
    // The generator's own shape rises in +Z from its own corner, so the node
    // is turned 180 degrees to climb the way the player walks (-Z), and its
    // pivot is put on the bottom back corner: then "where it stands" is the
    // foot of the stairs and nothing has to be halved by hand.
    var STAIR = { w: 1.20, rise: 4.35, run: 6.00, steps: 23, x: -3.2, z: 2.90 };
    var treppe = editor.add("Halle.Treppe", halle);
    node.setPos(treppe, STAIR.x, 0, STAIR.z);
    node.setNum(treppe, "rigidbody.motion", 0);
    if (has.blockout) {
        node.setNum(treppe, "blockout.kind", STAIRS);
        node.setVec(treppe, "blockout.size", STAIR.w, STAIR.rise, STAIR.run);
        node.setNum(treppe, "blockout.steps", STAIR.steps);
        node.setVec(treppe, "blockout.pivot", 0, -1, -1);   // foot, back edge
        node.setVec(treppe, "transform.rotation", 0, 180, 0);
    } else {
        node.setVec(treppe, "transform.scale", STAIR.w, STAIR.rise, STAIR.run);
    }
    INNEN.noPhysics(treppe);
    editor.setMaterial(treppe, M.ceiling);

    // What the player actually walks on: ONE ramp under the treads, invisible,
    // at the pitch of the flight.
    //
    // Not one box per step, which is the obvious version and was tried first:
    // a capsule has no step-up in this engine, so it walks into the 19 cm
    // riser of step one and stops there with the motor running. Every engine
    // that ships stairs puts a smooth collider over them for this reason - the
    // steps are what you see, the ramp is what you climb.
    var pitch = Math.atan2(STAIR.rise, STAIR.run);          // radians
    var rampLen = Math.sqrt(STAIR.rise * STAIR.rise + STAIR.run * STAIR.run);
    var rampT = 0.30;
    // Surface centre, then half a thickness down along the surface normal,
    // which after a rotation about X is (0, cos, sin).
    var ramp = INNEN.collider("Halle.Treppe.Rampe", halle,
        [STAIR.x,
         STAIR.rise * 0.5 - rampT * 0.5 * Math.cos(pitch),
         STAIR.z - STAIR.run * 0.5 - rampT * 0.5 * Math.sin(pitch)],
        [STAIR.w, rampT, rampLen]);
    node.setVec(ramp, "transform.rotation", pitch * 180 / Math.PI, 0, 0);

    // ------------------------------------------------------------- die Tueren
    // The leaves hang off a group at the origin rather than off their rooms,
    // and that is not tidiness: the runtime moves a node that HAS A BODY by
    // setting the body's transform, which is world space. A leaf parented into
    // a room 13 metres down the corridor would be moved to the world position
    // its script computed and land in the wrong room.
    var tueren = INNEN.group("Tueren", 0, [0, 0, 0]);
    var doorCount = 0;
    function door(name, pos, yaw, w, h, material, opts) {
        INNEN.door(name, tueren, pos, yaw, w, h, material, opts);
        doorCount++;
    }

    // The one that matters: you step into the box to make a call, and the door
    // shuts behind you and stays shut. It starts open because you just walked
    // through it.
    door("Tuer.Zelle", [0, 0, -(0.65 + 0.06)], 0, DOOR.width, DOOR.height, M.metal,
         { hinge: 1, openAngle: -95, speed: 260, startOpen: true,
           slamBehind: true, lockAfterSlam: true });

    // Hallway to hall: shut, not locked. E opens it - and until something
    // opens it, the corridor ends here.
    var hallDoorZ = flurZ - (flurDim.depth * 0.5 + flurDim.wall * 0.5);
    door("Tuer.Halle", [0, 0, hallDoorZ], 0, 1.10, 2.10, M.wall,
         { hinge: -1, openAngle: 90, speed: 130 });

    // The three doors off the hallway. All locked: they are there so the
    // corridor has doors, and so that trying them is answered.
    door("Tuer.Flur.Links1", [-(flurDim.width * 0.5 + flurDim.wall * 0.5), 0, flurZ + 2.6],
         90, 0.85, 1.98, M.wall, { hinge: 1, openAngle: -88, locked: true });
    door("Tuer.Flur.Links2", [-(flurDim.width * 0.5 + flurDim.wall * 0.5), 0, flurZ - 1.9],
         90, 0.85, 1.98, M.wall, { hinge: -1, openAngle: 88, locked: true });
    door("Tuer.Flur.Rechts", [(flurDim.width * 0.5 + flurDim.wall * 0.5), 0, flurZ + 0.4],
         90, 0.85, 1.98, M.wall, { hinge: 1, openAngle: 88, locked: true });

    // ------------------------------------------------------------ der Spieler
    // Standing in the box, a step back from the door, on the floor.
    var player = INNEN.player("Spieler", [0.0, 0.95, 0.30], 0);

    editor.commit();

    return JSON.stringify({
        rooms: [zelle, flur, halle],
        nodes: editor.count(),
        csg: has.blockout && has.csg,
        socket: has.socket,
        modifier: has.modifier,
        player: player,
        script: has.script,
        doors: doorCount,
        stairSteps: STAIR.steps,
        stairPitch: Math.round(pitch * 180 / Math.PI * 10) / 10,
        collider: has.collider,
        z: { zelle: 0, flur: flurZ, halle: halleZ }
    });
})();
