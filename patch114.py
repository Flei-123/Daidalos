import io

# ===========================================================================
# Freeze constraints. Talos has allowed_dofs already; nothing above it did.
# ===========================================================================
p = 'include/daidalos.h'
s = io.open(p, encoding='utf-8').read()
old = """    uint32_t   user_data;        /* free for the host                        */"""
new = """    /* Which degrees of freedom the solver may move. Unity's Constraints, and
     * the reason a platformer needs them: a capsule that may not TIP OVER is
     * one bit, and the alternative is fighting the solver with torque every
     * frame. 0 means "all six", so a body written before this existed behaves
     * exactly as it did. */
    uint32_t   frozen;           /* dai_freeze mask, 0 = nothing frozen       */
    uint32_t   user_data;        /* free for the host                        */"""
assert s.count(old) == 1, 'user_data field not found'
s = s.replace(old, new)

old = """/* ---- joints ------------------------------------------------------------ */"""
new = """/* What dai_body_desc::frozen can hold. A frozen axis is one the SOLVER may
 * not change; a script setting the transform still moves the body, which is
 * the same split Unity makes between "kinematic" and "constrained". */
typedef enum dai_freeze {
    DAI_FREEZE_POS_X = 1,
    DAI_FREEZE_POS_Y = 2,
    DAI_FREEZE_POS_Z = 4,
    DAI_FREEZE_ROT_X = 8,
    DAI_FREEZE_ROT_Y = 16,
    DAI_FREEZE_ROT_Z = 32,
    DAI_FREEZE_ALL   = 63,
    /* The two that get used by name: a 2D game locks the Z plane, an upright
     * character locks the two axes it could topple around. */
    DAI_FREEZE_2D      = DAI_FREEZE_POS_Z | DAI_FREEZE_ROT_X | DAI_FREEZE_ROT_Y,
    DAI_FREEZE_UPRIGHT = DAI_FREEZE_ROT_X | DAI_FREEZE_ROT_Z
} dai_freeze;

/* ---- joints ------------------------------------------------------------ */"""
assert s.count(old) == 1, 'joints anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

# --- Talos backend --------------------------------------------------------
p = 'src/physics_talos.cpp'
s = io.open(p, encoding='utf-8').read()
old = """    static tal_shape *make_shape(const dai_body_desc &d, const std::vector<dai_compound_part> &parts) {"""
new = """    // dai_freeze says what may NOT move; Talos says what MAY. One inversion,
    // in one place, with the bit order checked against the header next to it.
    static uint32_t dofs_from_frozen(uint32_t frozen) {
        uint32_t allow = TAL_DOF_ALL;
        if (frozen & DAI_FREEZE_POS_X) allow &= ~(uint32_t)TAL_DOF_TRANSLATION_X;
        if (frozen & DAI_FREEZE_POS_Y) allow &= ~(uint32_t)TAL_DOF_TRANSLATION_Y;
        if (frozen & DAI_FREEZE_POS_Z) allow &= ~(uint32_t)TAL_DOF_TRANSLATION_Z;
        if (frozen & DAI_FREEZE_ROT_X) allow &= ~(uint32_t)TAL_DOF_ROTATION_X;
        if (frozen & DAI_FREEZE_ROT_Y) allow &= ~(uint32_t)TAL_DOF_ROTATION_Y;
        if (frozen & DAI_FREEZE_ROT_Z) allow &= ~(uint32_t)TAL_DOF_ROTATION_Z;
        return allow;
    }

    static tal_shape *make_shape(const dai_body_desc &d, const std::vector<dai_compound_part> &parts) {"""
assert s.count(old) == 1, 'make_shape anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('daidalos.h + talos backend know about freezing')
