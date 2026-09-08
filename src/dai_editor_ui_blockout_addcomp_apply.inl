// MODULE 1 (blockout) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE the `default:` arm of the Add Component switch in
// src/dai_editor_ui.cpp, where `sel->kind` is the number this module's entries
// were pushed with and `ar2` is the node about to be written. One
// dai_doc_begin/commit pair around the whole switch is already open, so this
// only fills fields in - it does not touch the document.
//
// A component that arrives with zeros is a component the user cannot see: a
// Box of size 0 draws nothing and reads as "adding it did not work". So every
// shape arrives 1 m across, standing on its own floor, and the DoorSocket
// arrives at the size of a real door.
if (sel->kind >= 20 && sel->kind <= 24) {
    ar2.blockout = sel->kind - 20 + DAI_BLOCKOUT_BOX;
    if (ar2.blockout_size.x <= 0.0f)
        ar2.blockout_size = dai_vec3{ 1.0f, 1.0f, 1.0f };
    if (ar2.blockout_segments <= 0) ar2.blockout_segments = 16;
    if (ar2.blockout_steps <= 0)    ar2.blockout_steps = 8;
    // The mesh is built in metres, so the node must not scale it a second
    // time by its collider's half extent: render_extent 1,1,1 means "the
    // model is already the size it says it is". See dai_blockout_host.inl.
    ar2.render_extent = dai_vec3{ 1.0f, 1.0f, 1.0f };
    // Stairs and the arch are built standing on their own floor; a box a
    // room is made of is a wall, and a wall stands on the floor too.
    if (ar2.blockout == DAI_BLOCKOUT_STAIRS || ar2.blockout == DAI_BLOCKOUT_ARCH)
        ar2.blockout_pivot = dai_vec3{ 0.0f, -1.0f, 0.0f };
} else if (sel->kind == 25) {
    ar2.csg = DAI_CSG_SUBTRACT;
    ar2.render_extent = dai_vec3{ 1.0f, 1.0f, 1.0f };
} else if (sel->kind == 26) {
    ar2.door_socket = 1;
    if (ar2.door_width <= 0.0f)  ar2.door_width = 0.9f;
    if (ar2.door_height <= 0.0f) ar2.door_height = 2.05f;
    if (ar2.door_normal.x == 0.0f && ar2.door_normal.y == 0.0f && ar2.door_normal.z == 0.0f)
        ar2.door_normal = dai_vec3{ 0.0f, 0.0f, 1.0f };
}
