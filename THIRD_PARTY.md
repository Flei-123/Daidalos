# Third party material in Daidalos

No third party source code is committed to this repository. External
libraries are fetched at build time and keep their own licences:

| Library | Used for | Licence |
|---|---|---|
| Vulkan headers / loader | renderer (`thirdparty/win/vulkan-1.def`) | Apache-2.0 (headers), MIT-style loader |
| Talos | the physics engine (`src/physics_talos.cpp`) | MPL-2.0, same author |

The Daidalos source itself is GPL-3.0-or-later with the attribution term of
GPL section 7(b) -- see LICENSE and NOTICE.

Note: the engine no longer ships a Jolt backend. Physics is Talos, the
author's own deterministic engine, under the same licence.
