import io

p = 'src/dai_doc_text.cpp'
s = io.open(p, encoding='utf-8').read()

# ---- expand_one must follow the same rule as instantiate -------------------
old = """    graft(d, sub, n);
    dai_doc_destroy(sub);
    return true;
}"""
new = """    // The node that carries the reference IS the instance root - exactly the
    // rule dai_doc_prefab_instantiate follows. Grafting the WHOLE file under
    // it put the empty wrapper back on every load: place a prefab, save,
    // reopen, and the crate has grown a parent it did not have a second ago,
    // and every click selects that parent instead of the crate.
    //
    // The instance's own record is complete in the scene file - the save
    // skips the CHILDREN of an instance, not the instance - so only the
    // children have to come back.
    std::vector<dai_node> sids((size_t)dai_doc_count(sub));
    if (!sids.empty()) dai_doc_nodes(sub, sids.data(), (uint32_t)sids.size());
    graft_children(d, sub, sids.empty() ? 0 : sids[0], n);
    dai_doc_destroy(sub);
    return true;
}"""
assert s.count(old) == 1, 'expand_one graft call not found'
s = s.replace(old, new)

# ---- and the whole-tree graft has no callers left --------------------------
start = s.index("// Copies every node of `src` under `parent` in `dst`, keeping the shape of the")
end_marker = "// The chain of prefab files currently being expanded."
end = s.index(end_marker)
removed = s[start:end]
assert 'uint32_t graft(' in removed and 'expand_one' not in removed, 'cut window wrong'
s = s[:start] + s[end:]

io.open(p, 'w', encoding='utf-8').write(s)
print('expand_one fixed, dead graft() removed (%d chars)' % len(removed))
