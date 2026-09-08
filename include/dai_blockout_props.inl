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

if (!std::strcmp(name, "door.enabled"))       return flag(&r.door_socket, 0);
if (!std::strcmp(name, "door.offset"))        return vec(&r.door_offset);
if (!std::strcmp(name, "door.normal"))        return vec(&r.door_normal);
if (!std::strcmp(name, "door.width"))         return num(&r.door_width);
if (!std::strcmp(name, "door.height"))        return num(&r.door_height);
