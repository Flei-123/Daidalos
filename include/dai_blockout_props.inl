// MODULE 1 (blockout) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE prop_ref() in examples/editor_demo.cpp (and in any other
// host that answers component properties by name). In scope here:
//
//   dai_node_desc &r      the node being asked about
//   const char   *name    "blockout.width", "csg.op", "door.height", ...
//   num(float*) ival(int*) flag(int*, invert) vec(dai_vec3*) text(char*, size)
//                         the five ways to answer, exactly as the rows above
//
// This is what makes a new component reachable from a behaviour, from a C++
// component AND from the Jarvis bridge - all three go through this one table,
// which is why there is one table and not three.
//
// The names are the contract module 4 writes its JS against. Write them down
// in docs/BLOCKOUT.md the day they exist, and keep them in the completion
// table in src/dai_ui.cpp (AC_NODE) so the script editor offers them.

// The shape a node is, the boolean a CSG node runs, and the socket the next
// room docks against. `blockout.kind` is dai_blockout_kind (1 Box, 2 Cylinder,
// 3 Stairs, 4 Arch, 5 Wedge), `csg.op` is dai_csg_op (1 union, 2 subtract,
// 3 intersect). Sizes are FULL sizes in metres, pivot runs -1..1 per axis.
if (!std::strcmp(name, "blockout.kind"))      return ival(&r.blockout);
if (!std::strcmp(name, "blockout.enabled"))   return flag(&r.blockout, 0);
if (!std::strcmp(name, "blockout.size"))      return vec(&r.blockout_size);
if (!std::strcmp(name, "blockout.segments"))  return ival(&r.blockout_segments);
if (!std::strcmp(name, "blockout.steps"))     return ival(&r.blockout_steps);
if (!std::strcmp(name, "blockout.thickness")) return num(&r.blockout_thickness);
if (!std::strcmp(name, "blockout.pivot"))     return vec(&r.blockout_pivot);

if (!std::strcmp(name, "csg.op"))             return ival(&r.csg);
if (!std::strcmp(name, "csg.enabled"))        return flag(&r.csg, 0);

// The modifier stack, addressed by SLOT: "modifier.0.type" is the first entry
// in the list, "modifier.7" the last one there can be. The index is a single
// digit parsed straight out of the name - eight slots is DAI_MODIFIER_MAX and
// a two digit slot would be a slot that does not exist.
//
// Every field has the neutral name dai_modifier uses AND the name the type it
// belongs to calls it: `.width` and `.amount` are the same four bytes, not a
// copy, so a script that says `modifier.0.width = 0.05` and one that says
// `modifier.0.amount = 0.05` do the same thing and read each other back. The
// full list is in docs/BLOCKOUT.md.
if (!std::strncmp(name, "modifier.", 9)) {
    const char *mrest = name + 9;
    if (!std::strcmp(mrest, "count")) return ival(&r.modifier_count);
    if (mrest[0] >= '0' && mrest[0] < '0' + DAI_MODIFIER_MAX && mrest[1] == '.') {
        dai_modifier &m = r.modifiers[mrest[0] - '0'];
        const char *f = mrest + 2;
        if (!std::strcmp(f, "type"))      return ival(&m.type);
        if (!std::strcmp(f, "off"))       return flag(&m.off, 0);
        // The tick box in the inspector, the way round a reader expects it:
        // `enabled` is `off` upside down, like rigidbody.enabled and no_body.
        if (!std::strcmp(f, "enabled"))   return flag(&m.off, 1);
        if (!std::strcmp(f, "amount") || !std::strcmp(f, "width") ||
            !std::strcmp(f, "thickness") || !std::strcmp(f, "weld"))
                                          return num(&m.amount);
        if (!std::strcmp(f, "count") || !std::strcmp(f, "segments") ||
            !std::strcmp(f, "level") || !std::strcmp(f, "copies"))
                                          return ival(&m.count);
        if (!std::strcmp(f, "angle") || !std::strcmp(f, "rotation"))
                                          return num(&m.angle);
        if (!std::strcmp(f, "axis"))      return ival(&m.axis);
        if (!std::strcmp(f, "offset") || !std::strcmp(f, "step"))
                                          return vec(&m.offset);
        if (!std::strcmp(f, "param") || !std::strcmp(f, "shift"))
                                          return num(&m.param);
        if (!std::strcmp(f, "smooth"))    return flag(&m.smooth, 0);
        if (!std::strcmp(f, "relative"))  return flag(&m.relative, 0);
    }
}

if (!std::strcmp(name, "door.enabled"))       return flag(&r.door_socket, 0);
if (!std::strcmp(name, "door.offset"))        return vec(&r.door_offset);
if (!std::strcmp(name, "door.normal"))        return vec(&r.door_normal);
if (!std::strcmp(name, "door.width"))         return num(&r.door_width);
if (!std::strcmp(name, "door.height"))        return num(&r.door_height);
