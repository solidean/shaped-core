# Plan: carrying the old typed-geometry over

Status: **landed** for waves 1–3; polygon and polyline, and the items under *Left out*, remain.
The old typed-geometry is the reference for *what* the surface was, never for *how*: every geometric algorithm is reimplemented.
[geometry-query-matrix.md](geometry-query-matrix.md) is the binary-query machinery this builds on.

## The object model

**An object is its point set**, with a fixed representation; `pos` is the singleton set.
**An object type is maximal by default**: `aabb3` is the points in *and* on the box, `sphere3` the ball.

**A boundary is a type of its own**, one struct per family, never a template tag:
- `aabb_boundary<D, T>`, `sphere_boundary<D, DAmbient, T>`, …, with explicit `.boundary()` / `.solid()` between the two readings;
- 3D typedefs `aabb3_surface`, `sphere3_surface`, `box3_surface`, …;
- a **mantle** is a boundary without its flat caps: `cylinder_mantle`, `cone_mantle`, `hemisphere_mantle`, with `tube3` a typedef of the cylinder's.

The old tg instead had one struct with a trailing `boundary_tag` / `boundary_no_caps_tag` parameter, which made every verb branch on the tag.

**An embedded object** lives in a flat of lower dimension: `sphere<2, 3, T>` is a disk in 3D (`disk3`), `sphere_boundary<2, 3, T>` a circle in 3D (`circle3`), `box<2, 3, T>` a rectangle in 3D.

## The roster

| family | representation | boundary / mantle | wave |
|---|---|---|---|
| `pos` | the singleton set | — | 1 |
| `segment`, `ray`, `line` | as today | — | 1 |
| `triangle` | as today | — | 1 |
| `plane` | `{normal, dist}` | — | 1 |
| `halfspace` | `{normal, dist}`, the side `dot(n, x) <= dist` | `plane` is its boundary | 1 |
| `aabb` | `{min, max}` | `aabb_boundary` | 1 |
| `sphere` | `{center, radius}`; `<2, 3>` adds `normal` (disk3) | `sphere_boundary` (circle3) | 1 |
| `box` (oriented) | center plus half-extent matrix, columns the half-axes; `box<2, 3>` | `box_boundary` | 1 |
| `capsule` | `{segment axis, radius}` | `capsule_boundary` | 2 |
| `cylinder` | `{segment axis, radius}` | `cylinder_boundary`, `cylinder_mantle` (tube) | 2 |
| `cone` | `{apex, axis, radius}`, `axis` from the apex to the base center | `cone_boundary`, `cone_mantle` | 2 |
| `hemisphere` | `{center, radius, normal}` | `hemisphere_boundary`, `hemisphere_mantle` | 2 |
| `ellipsoid` | as today; `<2, 3>` is an ellipse in 3D | `ellipsoid_boundary` | 2 |
| `tetrahedron` | `{pos0..pos3}` | `tetrahedron_boundary` | 2 |
| `quad` | `{pos00, pos10, pos11, pos01}`, bilinear | — | 2 |
| `inf_cylinder`, `inf_cone` | `{line axis, radius}`, `{apex, dir, angle}` | `_boundary` | 3 |
| `frustum` | six planes; `.vertices()` returns the eight corners | `frustum_boundary` | 3 |
| `polygon`, `polyline` | needs a storage decision of its own | — | later |

**Representations that were choices.**
- **The box stores a half-extent matrix** so any affine image of a box is still a box; the axes need not be orthogonal.
- **The cone stores its apex** because that is how callers write a cone (a spotlight, a view cone); the old tg's cone was a pyramid over a disk.
- **The frustum stores planes only**, one canonical encoding; a caller that needs the corners repeatedly caches `.vertices()`.

**Not carried**: the generic `pyramid<BaseT>` (and `box_pyramid3`, `triangle_pyramid3`), and `inf_frustum`.
`tg::frustum` covers the infinite case instead, with an absent far plane, and support-based queries on it answer for the near rectangle.
A separate infinite frustum type is the way out if GJK on one is ever wanted: only the bounded type would have a support, so GJK on a far-less frustum becomes a compile error.
It was not done because `make_from_view_projection`'s result type would then depend on the matrix.

## Unary members

Per type, inline in the family's header, with no generic derivation.

| member | returns | old tg |
|---|---|---|
| `length()` / `perimeter()` / `area()` / `volume()` | `T`, by the measure table below | `area_of`, `volume_of`, `perimeter_of` |
| `centroid()` | `pos` | `centroid_of` |
| `bounds()` | `aabb<D, T>` | `aabb_of` |
| `boundary()` / `solid()` | the other reading | `boundary_of`, `solid_of` |
| `mantle()` / `caps()` | the mantle type / a `cc::fixed_array` of caps | `boundary_no_caps_of`, `caps_of` |
| `vertices()` / `edges()` / `faces()` | `cc::fixed_array<…, N>` | `vertices_of`, `edges_of`, `faces_of` |
| `normal()` / `plane()` | for planar objects only | `normal_of`, `plane_of` |
| `any_point()` | `pos` | `any_point` |
| `unbounded()` | the infinite extension: segment → line, cylinder → `inf_cylinder` | `inf_of` |

### Measures

| member | answered by | means |
|---|---|---|
| `length()` | intrinsic dim 1 | the length (`segment`, `circle3`) |
| `perimeter()` | intrinsic dim 2 | the boundary's length (`triangle`, `aabb2`, `disk3`) |
| `area()` | intrinsic dim 2 | the area (`triangle`, `aabb2`, `sphere3_surface`) |
| `area()` | intrinsic dim 3 | the boundary's area (`sphere3`, `aabb3`) |
| `volume()` | intrinsic dim 3 | the volume |

A boundary type answers only its own measures: `sphere3_surface.volume()` does not exist, and the enclosed volume is `s.solid().volume()`.
An ellipse's perimeter and an ellipsoid's surface area have no closed form, so `ellipsoid` has neither and `ellipsoid_boundary` has no measure at all.

## Parameters

`o.at(t)` maps a parameter to a point, and `o.parameter_of(p)` inverts it.
For a `p` off the object, `parameter_of` gives the parameter of `p.project_to(o)` on a segment, ray or line.
On a triangle, an aabb or a box it is the unclamped extension of the map: barycentrics go negative, box coordinates leave their range.

| family | parameter | domain | point |
|---|---|---|---|
| `segment` | `T t` | `[0, 1]` | `(1 - t)·pos0 + t·pos1` |
| `ray` | `T t` | `t >= 0` | `origin + t·dir` |
| `line` | `T t` | all of `T` | `origin + t·dir` |
| `triangle` | `comp<3, T>` barycentric | `b_i >= 0`, sum 1 | `b0·pos0 + b1·pos1 + b2·pos2` |
| `quad` | `comp<2, T>` | `[0, 1]²` | bilinear |
| `aabb` | `comp<D, T>` | `[0, 1]^D` | `min + c ⊙ (max - min)` |
| `box` | `comp<D, T>` | `[-1, 1]^D` | `center + H·c` |
| `tetrahedron` | `comp<4, T>` barycentric | `b_i >= 0`, sum 1 | as triangle |

## Sampling

`o.sample_uniform(rng)` for the finite objects, taking a `cc::random&`; widening it to any generator with `uniform(T, T)` is additive.
Not yet: the bilinear quad, an ellipsoid's surface, a frustum's faces, and a capsule's surface outside 3D.
The type says which set is sampled: `sphere3` the ball, `sphere3_surface` its surface, `cylinder_mantle` the tube alone.
**Direct methods first**, with a fixed number of draws per sample.
Where a direct method looks expensive, a nexus benchmark compares it against rejection sampling, and the faster one is kept.
The ball is that case: rejection from the enclosing cube measured 2.5x faster in 3D and 2.1x in 2D, so a ball's (and an ellipsoid's) sample draws a varying count.

## Support matrix

How each object answers, by query.
**C** is a closed form, **G** the GJK / EPA floor (any two objects marked G answer each other), **D** a derivation from another verb, **–** unsupported.
"Point" columns are a `pos` against the object; "Line" is a line, ray or segment against it.

| object | point project | point contains | convex pair (G) | Line parameters | notes |
|---|---|---|---|---|---|
| `pos`, `segment`, `triangle` | C | D | G | C (triangle; 2D linear pairs) | segment–segment closest points C |
| `ray`, `line` | C | D | – (unbounded) | – | |
| `plane`, `halfspace` | C | D | – (unbounded) | C | against anything with a support: intersects C, halfspace contains C |
| `aabb` | C | D | G | C | aabb–aabb, ball–aabb C; `aabb_boundary` project / parameters C |
| `box` | – (not orthogonal) | C | G | C (own frame) | box–box intersects C (SAT) |
| `sphere` (ball) | C | D | G | C | ball–ball C; surface C; disk line parameters C |
| `ellipsoid` | – | C | G | C (unit-ball frame) | |
| `capsule` | C | D | G | C | |
| `cylinder` | C | D | G | C | surface and tube C |
| `cone` | C (profile) | D | G | C | surface and mantle C |
| `hemisphere` | C | D | G | C | surface and dome C |
| `tetrahedron` | C | C | G | C | faces C |
| `quad` | – | – | – (not convex) | C | |
| `inf_cylinder`, `inf_cone` | C | D / C | – (unbounded) | C | inf_cylinder–ball intersects C |
| `frustum` | – | C | G | C | |

`distance_to` derives from closest points, which derive from a projection for a point and from GJK otherwise; `intersects` and `contains` for a point derive from the projection.
A boundary type meets an object when its solid does without containing it, wherever both of those answer.

## Left out

Close to the objects, and plausibly wanted soon — each needs a design of its own:
- **`rasterize`** — segment (Bresenham), triangle, integer circle, with a callback per covered integer position.
- **SAT** as a public `intersects_SAT(a, b, axes)` with per-object `shadow(axis)`; the GJK floor covers the generic case, and SAT lives on inside the box-box kernel.

Coming in as triangle members alongside the triangle's verbs, with no design beyond the naming rule: circumcircle, incircle, heights, `tbn_matrix` from a uv triangle.

Further away: Bezier curves, error quadrics (the mesh-simplification `quadric`), noise, colors, swizzling, and the `fwd_diff` / `interval` scalars.
