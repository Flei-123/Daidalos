// The example room, as DATA for the Jarvis bridge.
//
// Nothing about this room is compiled into anything. tools/modeling_shot.cpp
// reads this file and sends it down the bridge socket as one {"cmd":"eval"},
// which is exactly what tools/daibridge.py does when Jarvis asks for a room -
// so the picture in .gauntlet-shots/ is made by the same path a person or an
// AI uses, and not by a C++ function that draws a room nobody can change.
//
// Coordinates are metres, +Y up, the room's floor at y = 0. The door faces
// -Z, which is where the camera of the screenshots stands.
//
// It is written against the property names of the blockout components
// (module 1: "blockout.kind", "blockout.size", "csg.op", "door.width", ...).
// A build where those components do not exist yet answers the fallback for
// every unknown name, so the script ASKS before it relies on one: with them,
// the doorway is a CSG subtraction; without them it is three boxes around the
// same opening. Either way the room stands, and the day the components land
// the shot changes on its own.

(function () {
    var M = {
        floor:   "materials/pvc.daimat",
        wall:    "materials/raufaser.daimat",
        trim:    "materials/rost.daimat",
        ceiling: "materials/beton.daimat"
    };

    // ---- the room, in numbers, so changing it is changing data ------------
    var R = {
        width:  6.0,      // X, inner
        depth:  5.0,      // Z, inner
        height: 2.8,      // Y
        wall:   0.20,     // thickness
        door:   { width: 1.10, height: 2.05, x: 0.0 }
    };

    // One undo step for the whole room. Ctrl-Z after "build me a room" has to
    // mean the room, not the last of two hundred boxes.
    editor.begin("bridge: INNEN example room");

    function box(name, parent, pos, size, material) {
        var n = editor.add(name, parent || 0);
        node.setPos(n, pos[0], pos[1], pos[2]);
        // A fresh node is a 1 m box (half extent 0.5), so its scale IS its
        // size in metres - which is why "make me a box 2 x 3 x 1" is one
        // setVec and not a conversion nobody can check.
        node.setVec(n, "transform.scale", size[0], size[1], size[2]);
        node.setNum(n, "rigidbody.motion", 0);          // static: this is architecture
        if (material) editor.setMaterial(n, material);
        return n;
    }

    // Does this build know a component property? Asked on a scratch node and
    // never on a node that is kept, because the question WRITES.
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

    var root = editor.add("Room");
    node.setPos(root, 0, 0, 0);

    // ---- floor, ceiling, three solid walls --------------------------------
    box("Floor", root, [0, -R.wall * 0.5, 0],
        [R.width + 2 * R.wall, R.wall, R.depth + 2 * R.wall], M.floor);
    box("Ceiling", root, [0, R.height + R.wall * 0.5, 0],
        [R.width + 2 * R.wall, R.wall, R.depth + 2 * R.wall], M.ceiling);

    box("Wall.Back", root, [0, R.height * 0.5, R.depth * 0.5 + R.wall * 0.5],
        [R.width + 2 * R.wall, R.height, R.wall], M.wall);
    box("Wall.Left", root, [-(R.width * 0.5 + R.wall * 0.5), R.height * 0.5, 0],
        [R.wall, R.height, R.depth], M.wall);
    box("Wall.Right", root, [R.width * 0.5 + R.wall * 0.5, R.height * 0.5, 0],
        [R.wall, R.height, R.depth], M.wall);

    // ---- the wall with the doorway ----------------------------------------
    var frontZ = -(R.depth * 0.5 + R.wall * 0.5);
    var wallFront;
    if (hasBlockout && hasCsg) {
        // The real thing: a CSG node whose first child is the wall and whose
        // second is the opening, subtracted.
        wallFront = editor.add("Wall.Front", root);
        node.setPos(wallFront, 0, R.height * 0.5, frontZ);
        node.setNum(wallFront, "csg.op", 2);            // 0 union, 1 intersect, 2 subtract
        editor.setMaterial(wallFront, M.wall);

        var slab = editor.add("Wall.Front.Slab", wallFront);
        node.setNum(slab, "blockout.kind", 1);          // Box
        node.setVec(slab, "blockout.size", R.width + 2 * R.wall, R.height, R.wall);
        node.setPos(slab, 0, 0, 0);

        var hole = editor.add("Wall.Front.Doorway", wallFront);
        node.setNum(hole, "blockout.kind", 1);
        node.setVec(hole, "blockout.size", R.door.width, R.door.height, R.wall * 3);
        node.setPos(hole, R.door.x, -(R.height - R.door.height) * 0.5, 0);
    } else {
        // The same opening, out of the three pieces every builder cuts a door
        // out of. Not a stand-in for CSG: it is what a blockout looks like
        // before the CSG node exists, and it keeps the picture honest.
        wallFront = editor.add("Wall.Front", root);
        node.setPos(wallFront, 0, 0, 0);
        var side = (R.width + 2 * R.wall - R.door.width) * 0.5;
        box("Wall.Front.Left", wallFront,
            [-(R.door.width * 0.5 + side * 0.5) + R.door.x, R.height * 0.5, frontZ],
            [side, R.height, R.wall], M.wall);
        box("Wall.Front.Right", wallFront,
            [R.door.width * 0.5 + side * 0.5 + R.door.x, R.height * 0.5, frontZ],
            [side, R.height, R.wall], M.wall);
        box("Wall.Front.Lintel", wallFront,
            [R.door.x, R.door.height + (R.height - R.door.height) * 0.5, frontZ],
            [R.door.width, R.height - R.door.height, R.wall], M.wall);
    }

    // ---- the socket the room generator will dock the next room onto -------
    var socket = editor.add("DoorSocket.Front", root);
    node.setPos(socket, R.door.x, R.door.height * 0.5, frontZ);
    if (hasSocket) {
        node.setNum(socket, "door.width", R.door.width);
        node.setNum(socket, "door.height", R.door.height);
        node.setVec(socket, "door.normal", 0, 0, -1);
        // The socket IS its gizmo. The box every fresh node starts as would
        // stand in the doorway and hide the thing it marks.
        node.setVec(socket, "transform.scale", 0.05, 0.05, 0.05);
    } else {
        // No component yet, so the anchor is at least a visible marker with
        // the numbers on its name - never an invisible one.
        node.setVec(socket, "transform.scale", R.door.width, 0.06, 0.06);
        editor.setMaterial(socket, M.trim);
    }

    // ---- one lamp, so the room is lit from inside -------------------------
    var lamp = editor.add("Lamp", root);
    node.setPos(lamp, 0, R.height - 0.35, 0);
    node.setNum(lamp, "light.mode", 1);
    node.setNum(lamp, "light.intensity", 2.4);
    node.setNum(lamp, "light.range", 9.0);
    node.setVec(lamp, "light.color", 1.0, 0.93, 0.78);
    node.setNum(lamp, "rigidbody.enabled", 0);
    // A lamp is a light, not a crate: without this it also draws the 1 m box
    // every fresh node starts life as, right in the middle of the room.
    node.setVec(lamp, "transform.scale", 0.16, 0.06, 0.16);

    editor.commit();

    // The answer the caller gets back on the socket: what was built, in one
    // line, so a tool can check the room instead of trusting it.
    return JSON.stringify({
        room: root,
        nodes: editor.count(),
        csg: hasCsg && hasBlockout,
        socket: hasSocket,
        door: [R.door.width, R.door.height]
    });
})();
