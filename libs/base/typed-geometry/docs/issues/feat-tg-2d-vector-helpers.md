---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/slug_geometry.hh
  - symbol: tg::cross
  - symbol: tg::any_orthogonal
---

# 2D wedge product, a perpendicular with a promised turn, and midpoint / lerp of positions

Wanted: the 2D wedge product of two vectors, a perpendicular whose turn direction is promised, and a midpoint and lerp of two positions.

Slug's outline code lives on them.
The stroker offsets every curve along a perpendicular and tells a left turn from a right one by the wedge product.
Glyph compilation and the stroker both split quadratics at midpoints and interpolate between control points.

Today `tg::cross` is 3D only, and `tg::any_orthogonal` of a 2D vector turns left without promising which way.
sr's `impl/slug_geometry.hh` carries `cross`, `left_of`, `midpoint` and `lerp`, which `slug_path.cc` and `slug_shape.cc` share; they leave when tg gains them.
