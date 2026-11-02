---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_closure.sgl
---

# `geometry_coat_tangent` is absent

An anisotropic coat is stretched along the base's tangent spun onto the coat's normal.
So it follows the same uv layout the base does and cannot point its own way.
It is `geometry_tangent`'s mechanism a second time over, and nothing needs it yet.
