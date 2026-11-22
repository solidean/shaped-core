---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - issue: question-sgl-plain-vectors-as-layout
---

# float3 converts to vec3 with as, at no cost

`reflect` and `refract` take `vec3` alone, which stays: they are geometric operations.
A port of HLSL holds its directions in `float3`, and `d as vec3` is `no-matching-overload: float3 as vec3: no conversion`.
CHK-412 converts only the other way, and sends the way back through a constructor, `vec3(..v)`.

Wanted, reopening CHK-412's "only that direction": `float3 as vec3`, `float3 as pos3` and `float4 as hpos4`, each component kept.

- **It costs nothing**: no target text or IR beyond the value it converts, which a test pins on every target.
- Strong to strong stays explicit: `pos3 as vec3` is still refused.
- The diagnostic for a `float3` passed where a `vec3` stands suggests `as vec3`.
