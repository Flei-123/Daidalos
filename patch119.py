import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ===========================================================================
# Mehrfachbearbeitung. Unitys Regel: der Inspector zeigt das ERSTE Objekt und
# schreibt jedes Feld, das man anfasst, in ALLE.
# ===========================================================================
old = """    if (sel > 1) {
        dai_ui_label_fmt(p->ui, "%u nodes selected", sel);
        dai_ui_label(p->ui, "move them with the gizmo");
        return;
    }"""
new = """    if (sel > 1) {
        // Unity's rule, and the only workable one: show the FIRST object and
        // write every field you touch into all of them. The alternative -
        // "N objects selected", nothing editable - means setting the same
        // friction on twelve crates is twelve trips through the hierarchy.
        //
        // Fields that are not touched are not written, so eight crates keep
        // eight different positions until the moment you drag a position
        // field. That falls out of the diff at the bottom of this function:
        // it compares what the panel produced against what it started with.
        dai_ui_label_fmt(p->ui, "Editing %u objects - a field you change is written to all", sel);
        dai_ui_separator(p->ui);
    }"""
assert s.count(old) == 1, 'multi selection banner not found'
s = s.replace(old, new)

old = """    if (std::memcmp(&before, &r, sizeof(dai_node_desc)) != 0) {
        begin_field_tx(p, "Edit");
        dai_doc_set(d, n, &r);"""
new = """    if (std::memcmp(&before, &r, sizeof(dai_node_desc)) != 0) {
        begin_field_tx(p, "Edit");
        dai_doc_set(d, n, &r);
        // ...and the same CHANGE - not the same record - into the rest of the
        // selection. Copying the whole struct would give twelve crates one
        // position, one name and one parent, which is not editing twelve
        // crates, it is replacing eleven of them.
        if (sel > 1) {
            for (uint32_t si = 1; si < sel; ++si) {
                dai_node other = dai_editor_selected(p->ed, si);
                dai_node_desc t{};
                if (dai_doc_get(d, other, &t) != DAI_OK) continue;
                dai_node_desc t0 = t;
#define DAI_MF(field) \\
    if (std::memcmp(&before.field, &r.field, sizeof(r.field)) != 0) t.field = r.field
                // NOT name and NOT parent: those two are what makes an object
                // that object, and there is no reading of "multi-edit" where
                // eight objects should end up with one name.
                DAI_MF(tag);
                DAI_MF(position); DAI_MF(rotation); DAI_MF(scale);
                DAI_MF(shape); DAI_MF(motion); DAI_MF(half_extent); DAI_MF(trigger);
                DAI_MF(collider_center); DAI_MF(density); DAI_MF(friction);
                DAI_MF(restitution); DAI_MF(no_sleeping); DAI_MF(freeze);
                DAI_MF(no_body); DAI_MF(no_collider); DAI_MF(no_rigidbody);
                DAI_MF(script);
                DAI_MF(camera); DAI_MF(camera_fov); DAI_MF(camera_size);
                DAI_MF(light); DAI_MF(light_color); DAI_MF(light_range);
                DAI_MF(light_intensity); DAI_MF(light_cone);
                DAI_MF(sprite); DAI_MF(sprite_size);
                DAI_MF(text_on); DAI_MF(text); DAI_MF(text_size); DAI_MF(text_color);
                DAI_MF(text_anchor); DAI_MF(text_x); DAI_MF(text_y); DAI_MF(text_font);
                DAI_MF(audio_event); DAI_MF(audio_bus); DAI_MF(audio_volume);
                DAI_MF(audio_loop); DAI_MF(audio_autoplay);
                DAI_MF(mesh); DAI_MF(materials); DAI_MF(asset);
                DAI_MF(render_extent); DAI_MF(color); DAI_MF(roughness);
                DAI_MF(emissive); DAI_MF(render_flags);
                DAI_MF(hidden); DAI_MF(disabled);
#undef DAI_MF
                if (std::memcmp(&t0, &t, sizeof(t)) != 0) dai_doc_set(d, other, &t);
            }
        }"""
assert s.count(old) == 1, 'inspector write back not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('inspector edits the whole selection')
