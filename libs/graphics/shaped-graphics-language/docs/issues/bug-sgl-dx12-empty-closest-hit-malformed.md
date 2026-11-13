---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-shader-library/tests/sgl-shader-model-floor-test.cc
---

# SGL writes sgl_empty_closest_hit for dx12 with a signature DXC cannot parse

`sgl_empty_closest_hit` stands in for a hit group with no closest hit, which only metal builds ("a hit stage that takes no payload, which only metal builds", the DXIL route's gap).
The text route still writes it for dx12, with an empty first parameter where the payload would be:

```hlsl
void sgl_empty_closest_hit(, in BuiltInTriangleIntersectionAttributes sgl_attributes)
{
}
```

DXC refuses it at every profile: `expected parameter declarator`.
Seen in slib's floor sweep (`sgl-shader-model-floor-test.cc`, `k_not_compiled`), in every file that holds a hit group without a closest hit, each listed against this issue.
Either the text route refuses it on dx12 as the DXIL route does, or writes a closest hit that takes the payload and does nothing.
