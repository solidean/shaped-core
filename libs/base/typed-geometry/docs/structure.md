# typed-geometry structure proposal (tg::)

This is the living roadmap for typed-geometry. Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started

Update the tags as modules land. This document is design intent, not a guarantee of final API.

> **Layout deviation from the original proposal:** headers live in `src/typed-geometry/`
> (the shaped-core convention — `.hh`/`.cc` colocated under `src/<lib>/`), **not** in
> `include/typed-geometry/`. Include paths are unchanged: `#include <typed-geometry/...>`.

## Goals

- Strong semantic typing: `pos`, `vec`, `bivec`, `comp`, `mat`, `quat`, transforms, geometry objects.
- Prefer member functions for intrinsic/local operations and discoverability.
- Use free functions for symmetric, cross-type, heavy, or algorithmic operations.
- Keep include dependencies manageable.
- Avoid top-level dumping grounds like `algo/` or `util/`.
- Matrices are linear algebra objects, not transformation types.

## Top-Level Structure

```txt
src/typed-geometry/
  scalar/      [in progress]
  linalg/      [done for now]
  transform/   [planned]
  geometry/    [planned]
  curves/      [planned]
  color/       [planned]
  symbolic/    [planned]
  calculus/    [planned]
  sampling/    [planned]
  spatial/     [planned]
  mesh/        [planned]
```

## Dependency Direction

```txt
scalar
  -> linalg
    -> transform
    -> geometry
      -> curves
      -> spatial
      -> mesh

symbolic, calculus, sampling are cross-cutting but should avoid depending on mesh/spatial unless explicitly needed.
```

## Header Convention

```txt
foo.hh        type + cheap intrinsic member operations
foo_ops.hh    additional medium-weight operations
all.hh        full umbrella include for a module
module.hh     curated common include for day-to-day use
```

Example:

```txt
linalg/
  vec.hh
  vec_ops.hh
  linalg.hh
  all.hh
```

## Representation note (current implementation)

The implemented `comp` / `vec` / `pos` are a single generic type each (`vec<int D, class T>`),
not per-dimension specializations. Typedefs exist for D = 2/3/4 (`vec2f`, `vec3f`, `vec4f`, …
suffixes `f`=f32, `d`=f64, `i`=i32), but the types stay generic over `D`.

Storage is a public raw C array member named `data` (`T data[D]`). **There are no `.x/.y/.z`
members**, not even as accessor functions — components are reached via `data` or `operator[]`.
Default construction zero-initializes. Dimension-specific behavior is gated with `requires`.
We are deliberately seeing how far this minimal representation carries us.

## scalar/ [in progress]

Special scalar-like types and scalar traits.

```txt
scalar/
  traits.hh    [done]     scalar_traits<T> + tg::traits::has_sqrt/has_trigonometry/is_zero/is_one
  constants.hh [done]     tg::pi<T> (more to come)
  scalar.hh    [done]     tg::one/sqrt + trig (sin/cos/tan/sin_cos/sec/csc/cot, asin/acos/atan/atan2)
  angle.hh     [done]     angle<T> domain newtype + _rad_/_deg_ literals
  all.hh       [done]
  complex.hh   [planned]
  interval.hh  [planned]
  fwd_diff.hh  [planned]
  rev_diff.hh  [planned]
  error.hh     [planned]
```

`scalar_traits<T>` is the extensibility seam: every scalar capability (`has_sqrt`,
`has_trigonometry`, `one`, `is_zero`, `is_one`, `sqrt`, `sin`, `cos`, `atan2`) routes through it, so
custom scalar types (expression trees, double-double, bigint/bigrat) can opt in by specializing the
trait — `is_zero`/`is_one` in particular let symbolic/exact scalars give a smarter answer than a raw
comparison. tg avoids `std::` type-traits / `<cmath>` in user-facing code for this reason.
`f32`/`f64` are fully featured; every integer type (incl. `signed`/`unsigned char` but **not** plain
`char`) gets `one`/`is_zero`/`is_one`; `bool` has its own specialization. `bigint`/`bigrat` are
expected to live here too (useful as scalars outside symbolic algebra).

`angle<T>` stores radians, is built only via `make_from_radians`/`make_from_degree`, read via
`.radians()`/`.degree()`, and has `_rad_f`/`_rad_d`/`_deg_f`/`_deg_d` literals (e.g. `90_deg_f`).
It supports addition and scalar multiplication with no wrap-around.

## linalg/ [in progress]

Algebraic building blocks.

```txt
linalg/
  vec.hh           [done]     + zero, make_unit
  vec_ops.hh       [done]     dot, normalize
  pos.hh           [done]     + zero
  pos_ops.hh       [done]     distance, distance_sqr
  comp.hh          [done]     storage + access only; arithmetic is planned (see below)
  bivec.hh         [done]     + zero; C(D,2) components (3D order {yz, zx, xy})
  cross.hh         [done]     cross, dual, undual (3D; the {yz,zx,xy} order makes dual the identity)
  mat.hh           [done]     col-major, zero/identity, rotations, products
  quat.hh          [done]     zero/identity, rotations, products, axis()/angle()
  linalg.hh        [done]
  all.hh           [done]

  norm.hh          [planned]
  normalize.hh     [planned]
  decomposition.hh [planned]
```

Important semantic rules:

```cpp
pos - pos -> vec          // [done]
pos + vec -> pos          // [done]
vec + vec -> vec          // [done]
pos + pos -> pos          // [done]  translation of the singleton point set

cross(vec3, vec3) -> bivec3   // [done]
dual(bivec3)      -> vec3     // [done]  explicit Euclidean 3D escape hatch
undual(vec3)      -> bivec3   // [done]  explicit pseudovector-to-bivector conversion
```

`mat` is not a transform type. It is linear algebra data.

Special values are exposed as static constants (`vec::zero`, `pos::zero`, `comp::zero`,
`bivec::zero`, `mat::zero`, `mat::identity`, `quat::zero`, `quat::identity`). They are runtime
constants (a static member can't be `constexpr` of its own incomplete type), not constant
expressions. All factory methods are named `make_*` (e.g. `vec::make_unit`, `make_from_values`,
`mat::make_rotation_z`).

`comp` is the "semantics-free" building block in the sense that it is the raw component-wise
type. It is therefore the eventual home of **all** component-wise arithmetic (mul, div, min/max,
… element-wise). For now it only stores and indexes; the arithmetic is **[planned]**.

`pos` is special: it is both a linalg and a geometric object. Geometric objects are sets of
points, and `pos` is a singleton set — so `pos + pos -> pos` because adding translates the
`{pos}` object. We treat `+ vec` and `+ pos` as applying a translation.

Matrices will be **column-major** stored, with C++23 multi-argument `operator[]` for element
access (lands with `mat`).

## transform/ [planned]

Semantic transformation types.

```txt
transform/
  rigid_transform.hh
  similarity_transform.hh
  affine_transform.hh
  projective_transform.hh

  transform_ops.hh
  compose.hh
  inverse.hh

  transform.hh
  all.hh
```

Examples:

```cpp
rigid_transform3f r;
affine_transform3f a;

transform(t, pos)
transform(t, vec)
transform(t, bivec)
```

Rules:

```cpp
mat3f m;                // linear algebra object
affine_transform3f t;   // semantic transform

transform(t, p);        // OK
m * p;                  // probably not OK
```

## geometry/ [planned]

Geometric primitives and geometric queries.

```txt
geometry/
  primitives/   # aabb, obb, sphere, plane, triangle, segment, ray, line, polygon, ...
  query/        # distance, projection, closest, intersection, intersects, containment, ...
  measure/      # area, volume, centroid, bounds, moments
  construct/    # hull, fitting, primitives_from_points
  geometry.hh
  all.hh
```

Member functions stay intrinsic/cheap (e.g. `ray.at(t)`, `aabb.center()`, `aabb.contains(p)`,
`triangle.area()`). Symmetric/cross-type queries are free functions (`distance(a, b)`,
`closest_point(p, primitive)`, `intersects(a, b)`, `intersection(a, b)`, `contains(a, b)`).
Avoid making every pairwise query a member.

## curves/ [planned]

Parametric curves and surfaces (`curve`, `surface`, `bezier`, `spline`, `nurbs`, `polycurve`;
`evaluation`, `derivatives`, `fitting`, `subdivision`).

```cpp
curve.eval(t)
curve.derivative(t)
surface.eval(u, v)
surface.normal_bivec(u, v)
```

## color/ [planned]

Color values and color spaces (`srgb`, `linear_rgb`, `xyz`, `lab`, `hsv`; `conversion`, `gamut`).

```cpp
srgb_color c;
linear_rgb_color l = convert<linear_rgb_color>(c);
```

## symbolic/ [planned]

Basic CAS and exact symbolic algebra (`bigint`, `bigrat`, `monomial`, `polynomial`,
`rational_polynomial`, `expr`; `simplify`, `factor`, `substitute`, `derivative`, `evaluate`).

Notes:

- Some symbolic types are also valid scalar types.
- Keep symbolic independent from geometry where possible. Geometry may instantiate over symbolic
  scalars, but symbolic should not know geometry.
- `bigint`/`bigrat` likely belong to `scalar/` because they are useful as scalars outside of
  symbolics.

## calculus/ [planned]

Differentiation, integration, and optimization (`autodiff`, `fwd_diff`, `rev_diff`, `integrate`,
`optimize`, `root_find`, `minimization`). Thin wrappers may re-export scalar AD types. Heavy
optimization/integration algorithms live here, not in a top-level `algo/`.

## sampling/ [planned]

Sampling algorithms and distributions (`random`, `distribution`, `low_discrepancy`, `blue_noise`;
domain samplers for sphere/hemisphere/disk/triangle/polygon/mesh).

```cpp
sample_sphere(rng)
sample_triangle(tri, rng)
sample_mesh(mesh, rng)
```

## spatial/ [planned]

Acceleration structures and space partitioning (`bvh`, `kd_tree`, `grid`, `hash_grid`, `octree`;
`builder`, `queries`, `traversal`).

```cpp
bvh<Triangle> b;
b.intersect(ray)
b.closest(p)
```

## mesh/ [planned]

Mesh data structures and mesh-domain algorithms (`core/`, `polygon/`, `triangle/`, `halfedge/`,
`attributes/`, `algorithms/`, `io/`). Mesh-specific algorithms (triangulate, rasterize, remesh,
simplify, repair, weld, smooth, subdivide, boolean support, parameterize) live here, not at the
top level.

## Member vs Free Function Rule

Use members for intrinsic, local, discoverable operations:

```cpp
v.length()        v.normalized()        q.inverse()
ray.at(t)
box.center()      box.extents()         box.contains(p)
tri.area()        tri.area_bivec()      tri.centroid()      tri.bounds()
```

Use free functions for symmetric, cross-type, heavy, or extensible operations:

```cpp
distance(a, b)    closest_points(a, b)  intersects(a, b)    intersection(a, b)
project(p, primitive)
triangulate(poly) rasterize(mesh, target) sample(shape, rng) optimize(problem) integrate(f, domain)
```

## Umbrella Include Policy

Each module provides a curated common include (`module/module.hh`) and a complete, potentially
expensive one (`module/all.hh`). The top-level `<typed-geometry/all.hh>` pulls in everything.

```cpp
#include <typed-geometry/linalg/linalg.hh> // common vec/pos/comp (+ bivec/mat/quat once they land)
#include <typed-geometry/linalg/all.hh>    // everything in linalg
#include <typed-geometry/all.hh>           // everything (expensive)
```

## Initial Implementation Order

```txt
1.  scalar traits/constants            [in progress]  traits, constants, one/sqrt/sin/cos, angle done
2.  linalg: vec, pos, comp             [done]         (comp arithmetic still planned)
3.  linalg: bivec + cross/dual/undual  [done]
4.  linalg: mat, quat                  [done]
5.  transform: rigid/affine + transform(pos/vec/bivec)   [planned]
6.  geometry primitives: aabb, ray, segment, triangle, plane   [planned]
7.  geometry measure/query basics      [planned]
8.  curves                             [planned]
9.  symbolic scalars                   [planned]
10. calculus                           [planned]
11. sampling                           [planned]
12. spatial                            [planned]
13. mesh core                          [planned]
14. mesh algorithms                    [planned]
```
