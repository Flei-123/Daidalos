// A staircase out of ONE step, built over the Jarvis bridge.
//
//   tools/daibridge.py eval examples/scripts/modifier_stair.js
//
// The point of the modifier stack is that detail is a RULE, not a hundred
// placed vertices. This script is that claim in twenty lines: one blockout box
// the size of a single tread, a Bevel that breaks its edges, and an Array that
// repeats it up and back. Change `steps` and the staircase is a different
// staircase - nothing else in the file moves.
//
// The ORDER of the two entries matters and is chosen on purpose: the bevel is
// FIRST, so every one of the steps carries its own broken edge. Swap them and
// the bevel would see one long block of treads and only break the outline of
// it - which is a different, also useful, thing.
//
// Written against the property names in include/dai_blockout_props.inl, the
// ones docs/BLOCKOUT.md lists: "modifier.count", "modifier.N.type" and then
// the fields of that type by their own names ("width", "segments", "copies",
// "offset"). A build without the stack answers the fallback for every unknown
// name, so the script ASKS first and falls back to one plain box per step -
// the staircase stands either way.

(function () {
    var S = {
        name:   "Stair",
        steps:  9,
        tread:  0.30,     // Z, how deep one step is
        rise:   0.18,     // Y, how high one step is
        width:  1.30,     // X
        bevel:  0.012,    // the break on the nosing, in metres
        pos:    [0, 0, 0],
        material: "materials/beton.daimat"
    };

    // Does this build know a property? Asked on a scratch node, because the
    // question WRITES - the same probe innen_room.js uses.
    function supports(prop) {
        var probe = editor.add("__probe");
        node.setNum(probe, prop, 3);
        var known = node.getNum(probe, prop) === 3;
        editor.remove(probe);
        return known;
    }

    var hasBlockout  = supports("blockout.kind");
    var hasModifiers = supports("modifier.count");

    // One undo step for the whole staircase: Ctrl-Z after "build me a stair"
    // has to mean the stair.
    editor.begin("bridge: a stair from one step");

    var stair;
    if (hasBlockout && hasModifiers) {
        stair = editor.add(S.name);
        node.setPos(stair, S.pos[0], S.pos[1], S.pos[2]);
        node.setNum(stair, "rigidbody.motion", 0);       // static: architecture
        node.setNum(stair, "blockout.kind", 1);          // Box
        node.setVec(stair, "blockout.size", S.width, S.rise, S.tread);
        node.setVec(stair, "blockout.pivot", 0, -1, 0);  // stands on the floor

        node.setNum(stair, "modifier.count", 2);

        // 1: break the edges of the tread. 30 degrees, so only the real
        // corners are broken and nothing flat is touched.
        node.setNum(stair, "modifier.0.type", 1);        // Bevel
        node.setNum(stair, "modifier.0.width", S.bevel);
        node.setNum(stair, "modifier.0.segments", 2);
        node.setNum(stair, "modifier.0.angle", 30);

        // 2: repeat it, one rise up and one tread back per copy.
        node.setNum(stair, "modifier.1.type", 4);        // Array
        node.setNum(stair, "modifier.1.copies", S.steps);
        node.setVec(stair, "modifier.1.offset", 0, S.rise, S.tread);
    } else {
        // No stack in this build: the same staircase as one box per step. It
        // is not a stand-in for the modifiers - it is what building it by
        // hand costs, which is the comparison this file is about.
        stair = editor.add(S.name);
        node.setPos(stair, S.pos[0], S.pos[1], S.pos[2]);
        node.setNum(stair, "renderer.enabled", 0);
        node.setNum(stair, "rigidbody.enabled", 0);
        for (var i = 0; i < S.steps; ++i) {
            var step = editor.add(S.name + ".Step" + (i + 1), stair);
            node.setPos(step, 0, S.rise * (i + 0.5), S.tread * i);
            node.setVec(step, "transform.scale", S.width, S.rise, S.tread);
            node.setNum(step, "rigidbody.motion", 0);
            if (S.material) editor.setMaterial(step, S.material);
        }
    }
    if (S.material) editor.setMaterial(stair, S.material);

    editor.commit();

    return S.name + ": " + S.steps + " steps, " +
           (hasModifiers ? "one box and two modifiers"
                         : "one box per step (no modifier stack in this build)");
})();
