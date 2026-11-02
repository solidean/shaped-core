---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/resolve.hh
  - symbol: sv::resolved_attribute
  - symbol: sv::resolve_material
---

# A sampled attribute cannot say what it is sampled THROUGH

`resolved_attribute::uv` is one hardcoded field: a `float2` mesh attribute, found by name, and nothing else may play that role.
The generalization is a *dependent* attribute: one that declares which other attribute supplies its coordinate.
A 1D coordinate into a 1D texture, a second uv set and triplanar then become the same mechanism rather than three special cases.
It is a change to `resolved_attribute`, to the two shape-side hashes in `resolve_material`, and to the slot the generator emits for it.
