---
created: 2026-11-02
parent: epic-sgl-metal-raytracing-dxr-parity
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
closed: 2026-11-02
resolution: fixed
---

# metal: `accept_and_end_search` in an any hit

On dx12 and vulkan, an any hit's `accept_and_end_search` commits the hit and stops the traversal.
On metal it acts as `accept`: an any hit is a triangle intersection function, which returns accept or not and has no way to end the search.

Candidates, none tried:

- **A ray type whose any hit only ever ends the search** (a shadow ray) traces with an intersector set to accept any intersection, which is that semantics exactly.
  The check pass can tell when every accepting path of a ray type's any hits ends the search.
- **A flag in the payload** that later intersection functions read and reject on.
  That stops further work from intersection functions, but not a closer opaque triangle, which metal accepts without calling one: so it is not DXR's semantics, only a cost saving.
- **Refusing** `accept_and_end_search` on metal where neither applies, as `target-lacks-feature`, rather than silently accepting.

Whichever lands, a silent difference from DXR is the one outcome portability by default excludes.
