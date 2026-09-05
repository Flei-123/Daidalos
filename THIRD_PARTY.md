# Third party material in Daidalos

No third party source code is committed to this repository. External
libraries are fetched at build time and keep their own licences:

| Library | Used for | Licence |
|---|---|---|
| Jolt Physics | optional physics backend (`tools/build_jolt_win.sh`) | MIT |
| Vulkan headers / loader | renderer (`thirdparty/win/vulkan-1.def`) | Apache-2.0 (headers), MIT-style loader |
| Talos | the default physics backend (`src/physics_talos.cpp`) | GPL-3.0-or-later, same author |

The Daidalos source itself is GPL-3.0-or-later with the attribution term of
GPL section 7(b) -- see LICENSE and NOTICE.

Note on Jolt: because Jolt is MIT, it may be linked into a GPL-3.0 program
without conflict. The reverse is not true -- a closed product may not take
Daidalos just because Jolt is permissive.
