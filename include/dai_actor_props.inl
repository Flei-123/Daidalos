// The properties an ACTOR needs, for the hosts that answer components by name.
// Statement seam, exactly like include/dai_blockout_props.inl - included INSIDE
// prop_ref(), with `dai_node_desc &r`, `const char *name` and the five answer
// helpers (num/ival/flag/vec/text) in scope.
//
// WHY THIS FILE EXISTS: a room can be built over the bridge already, but a
// PLAYER cannot. Placing one means saying "this node carries this behaviour"
// and "its collider is a capsule this big, sitting here" - and neither the
// script list nor the collider shape had a name in the table, so both were
// reachable from the inspector and from nowhere else. Everything the editor
// can do to a node, the bridge has to be able to do, or building a level
// without a mouse stops at the walls.
//
// Kept out of the two hosts' copies of the table (examples/editor_demo.cpp and
// tools/modeling_shot.cpp) for the reason the blockout file gives: one table,
// so a getter and a setter cannot disagree.

// The behaviours on this node, ';' separated - the way the document stores
// them, and the way Unity stacks script components:
//
//     node.setStr(p, "script", "innen_player.js");
//     node.setStr(p, "script", "innen_player.js;footsteps.js");
//
// The path is resolved against the project's Assets folder, which is what the
// inspector's own script field hands over.
if (!std::strcmp(name, "script"))               return text(r.script, sizeof(r.script));
if (!std::strcmp(name, "script.file"))          return text(r.script, sizeof(r.script));

// The collider. `collider.shape` is dai_shape (0 box, 1 sphere, 2 capsule,
// 3 compound, 4 cylinder); the size comes from transform.scale, the way it
// does in the inspector, and `collider.center` offsets the shape from the
// node's origin - which is how a capsule stands on the floor while its
// transform sits at the feet.
if (!std::strcmp(name, "collider.shape"))       return ival(&r.shape);
if (!std::strcmp(name, "collider.center"))      return vec(&r.collider_center);
// "enabled" is no_collider upside down, like rigidbody.enabled and no_body:
// a node WITH a collider is the normal case, and a script should not have to
// spell a double negative.
if (!std::strcmp(name, "collider.enabled"))     return flag(&r.no_collider, 1);

// Unity's Constraints, as a dai_freeze mask (1 posX, 2 posY, 4 posZ, 8 rotX,
// 16 rotY, 32 rotZ; 40 = DAI_FREEZE_UPRIGHT). Without a name here a capsule
// with a player on it topples over the first door sill and no script can stop
// it, because the solver owns the rotation.
//
// The field is a uint32_t and the table answers ints; the mask never uses the
// top bit, so the cast is exact for every value the editor can make.
if (!std::strcmp(name, "rigidbody.freeze"))
    return ival(reinterpret_cast<int *>(&r.freeze));
