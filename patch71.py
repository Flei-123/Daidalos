#!/usr/bin/env python3
# patch71 - the README still said Jolt was what runs. It has not been for a
# while: Talos is the shipped backend, Jolt is the reference the tests measure
# against, and the Windows build does not carry it at all.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p71'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('README.md')

s = sub1(s,
"""  physics      swappable backend (Jolt today, null backend for proof)""",
"""  physics      swappable backend (Talos ships; Jolt as reference, null for proof)""",
    'stack line')

s = sub1(s,
"""`build.sh` compiles `dai_engine.cpp` **without the Jolt include path**. If a
Jolt header ever leaks into the engine core, the build breaks. That is the
entire point of `src/dai_physics.hpp`.""",
"""`build.sh` compiles `dai_engine.cpp` **without any backend's include path** -
not Jolt's, not Talos's. If a backend header ever leaks into the engine core,
the build breaks. That is the entire point of `src/dai_physics.hpp`, and it is
what made swapping the default from Jolt to Talos a link-line change rather
than a rewrite.""",
    'leak test paragraph')

s = sub1(s,
"""Jolt (physics) and Aulos (audio) are vendored. Vulkan is an API, not a library
that does work for us.""",
"""Aulos (audio) is vendored. Talos - the physics engine that ships in the editor
- is a sibling project built alongside this one; Jolt is vendored too but is
opt-in now (`WITH_JOLT=1`), kept as the reference implementation the physics
tests compare against rather than as something the binary carries. The Windows
editor links Talos and the null backend only, which is what it says on start-up.
Vulkan is an API, not a library that does work for us.""",
    'dependencies paragraph')

s = sub1(s,
"""One interface, no foreign types: `dai_vec3`, `dai_quat`, slot indices. Two
implementations: `physics_jolt.cpp` (the only file in the project that includes
Jolt) and `physics_null.cpp` (gravity and a floor - it exists so the abstraction
can be proven, not assumed).""",
"""One interface, no foreign types: `dai_vec3`, `dai_quat`, slot indices. Three
implementations:

* `physics_talos.cpp` - **what ships.** Talos is this project's own solver, and
  the only backend the editor you download contains.
* `physics_jolt.cpp` - the only file that includes Jolt. Opt-in (`WITH_JOLT=1`),
  and worth keeping: an independent implementation is the only honest way to
  tell "our solver is right" from "our solver and our test agree".
* `physics_null.cpp` - gravity and a floor. It exists so the abstraction can be
  proven, not assumed.

Two backends that both pass the same suite is the reason the interface is
trusted; a third that does almost nothing is the reason it is known to be an
interface at all.""",
    'backend section')

s = sub1(s,
"""*Jolt for Windows.* `tools/build_jolt_win.sh`, once. Two traps in there, both""",
"""*Jolt for Windows.* Only needed with `WITH_JOLT=1` - the shipped editor uses
Talos and does not link Jolt at all. Kept here because the traps are real and
cost a day each. `tools/build_jolt_win.sh`, once. Two traps in there, both""",
    'jolt windows section')
wr('README.md', s)
print('patch71 ok')
