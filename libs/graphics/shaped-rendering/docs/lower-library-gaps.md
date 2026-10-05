# What shaped-rendering wants from the lower libraries

The repo rule is that a task best served by **extending a lower library** says so rather than work around it silently
(see [CLAUDE.md](../../../../CLAUDE.md), *The libraries are living — surface missing pieces*).
This file is where shaped-rendering keeps those findings.
Each entry names what is wanted, why, and what the code does today instead.
**Retire an entry in the same change that lands the capability**, and delete the workaround it describes.

---

## typed-geometry

### 2D vector and position helpers

**Wanted:** the 2D wedge product of two vectors, a perpendicular whose turn is promised, and a midpoint and lerp of two positions.

**Why:** Slug's outline code lives on them.
The stroker offsets every curve along a perpendicular and tells a left turn from a right one by the wedge product.
Glyph compilation and the stroker both split quadratics at midpoints and interpolate between control points.

**Today:** `tg::cross` is 3D only, and `tg::any_orthogonal` of a 2D vector turns left without promising which way.
[impl/slug_geometry.hh](../src/shaped-rendering/impl/slug_geometry.hh) carries `cross`, `left_of`, `midpoint` and `lerp`, which `slug_path.cc` and `slug_shape.cc` share.
