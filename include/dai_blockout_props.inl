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

/* module 1 fills this in, e.g.:
 *   if (!std::strcmp(name, "blockout.kind"))  return ival(&r.blockout);
 *   if (!std::strcmp(name, "blockout.size"))  return vec(&r.blockout_size);
 */
