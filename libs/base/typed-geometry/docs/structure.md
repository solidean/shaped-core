# typed-geometry module roadmap

The living roadmap for typed-geometry.
Section headers carry a status tag: **[done]** implemented and tested, **[in progress]** partially implemented, **[planned]** not started.
Update the tags as modules land.
This is design intent, not a guarantee of final API.

Headers live in `src/typed-geometry/` — the shaped-core convention of colocated `.hh`/`.cc` under `src/<lib>/` — and are included as `#include <typed-geometry/...>`.

## Goals

- Strong semantic typing: `pos`, `vec`, `bivec`, `comp`, `mat`, `quat`, transforms, geometry objects.
- Keep include dependencies manageable.
- Avoid top-level dumping grounds like `algo/` or `util/`.

The member-versus-free-function split lives in the [coding-guidelines](coding-guidelines.md), and "a `mat` is not a transform" in [modules/linalg.md](modules/linalg.md).
The same guidelines own the storage rule (`T data[D]`, no `.x/.y/.z`), one-generic-type-per-family and the `make_*` naming.
This file is the roadmap only — [docs/_index.md](_index.md) is the doc map, and the [readme](../readme.md) the front door.

## Top-Level Structure

```txt
src/typed-geometry/
  scalar/      [in progress]
  linalg/      [in progress]
  transform/   [planned]
  geometry/    [in progress]
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

[modules/scalar.md](modules/scalar.md) covers the `scalar_traits` seam, which types count as scalars, and the `angle<T>` contract.

## linalg/ [in progress]

Algebraic building blocks.

```txt
linalg/
  vec.hh           [done]     + zero, make_unit
  vec_ops.hh       [done]     dot, normalize
  pos.hh           [done]     + zero
  pos_ops.hh       [done]     distance, distance_sqr
  comp.hh          [done]     storage + access + full component-wise arithmetic
  comp_ops.hh      [done]     component-wise min/max
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

[modules/linalg.md](modules/linalg.md) covers the rest — why `pos + pos` translates, why `comp` owns all component-wise arithmetic, and why a `mat` is data rather than a transform.
`mat` is column-major, with the C++23 multi-argument `operator[]` for element access.

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

## geometry/ [in progress]

Geometric primitives and geometric queries.
The primitive *types* and the `object_traits` seam have landed; queries, measures and construction are still planned.

```txt
geometry/
  traits.hh         [done]     object_traits<ObjT> seam + tg::traits::intrinsic_dim/ambient_dim/is_finite
  primitives/       [in progress]
    aabb.hh         [done]     solid axis-aligned box {min..max}
    triangle.hh     [done]     filled triangle (3 verts)
    segment.hh      [done]     closed segment between 2 endpoints
    ray.hh          [done]     {origin + t*dir : t >= 0}
    line.hh         [done]     {origin + t*dir : t in R}
    plane.hh        [done]     hyperplane {x : dot(normal,x) == dist}
    primitives.hh   [done]
    # planned: obb, sphere, polygon, ...
  query/            [planned]  # distance, projection, closest, intersection, intersects, containment, ...
  measure/          [planned]  # area, volume, centroid, bounds, moments
  construct/        [planned]  # hull, fitting, primitives_from_points
  geometry.hh       [done]
  all.hh            [done]
```

Every primitive denotes a **set of points**, classified by an `object_traits` specialization colocated with the type.
[modules/geometry.md](modules/geometry.md) covers what those facts mean, and why representation is not interpretation.

Queries are intentionally **not** implemented yet — the representations settle first.
When they land, members stay intrinsic and cheap (`ray.at(t)`, `aabb.center()`, `triangle.area()`).
Symmetric or cross-type queries are free functions: `distance(a, b)`, `intersection(a, b)`.
[plans/geometry-query-matrix.md](plans/geometry-query-matrix.md) is the agreed shape for that layer.

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
- Keep symbolic independent from geometry where possible.
  Geometry may instantiate over symbolic scalars, but symbolic must not know geometry.
- `bigint`/`bigrat` likely belong to `scalar/`, since they are useful as scalars outside symbolic algebra.

## calculus/ [planned]

Differentiation, integration and optimization (`autodiff`, `fwd_diff`, `rev_diff`, `integrate`, `optimize`, `root_find`, `minimization`).
Thin wrappers may re-export scalar AD types.
Heavy optimization and integration algorithms live here, not in a top-level `algo/`.

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

## Umbrella Include Policy

Each module provides a curated common include (`module/module.hh`) and a complete, potentially expensive one (`module/all.hh`).
The top-level `<typed-geometry/all.hh>` pulls in everything.

```cpp
#include <typed-geometry/linalg/linalg.hh> // the common linalg types
#include <typed-geometry/linalg/all.hh>    // everything in linalg
#include <typed-geometry/all.hh>           // everything (expensive)
```

## Initial Implementation Order

```txt
1.  scalar traits/constants            [in progress]  traits, constants, one/sqrt/sin/cos, angle done
2.  linalg: vec, pos, comp             [done]
3.  linalg: bivec + cross/dual/undual  [done]
4.  linalg: mat, quat                  [done]
5.  transform: rigid/affine + transform(pos/vec/bivec)   [planned]
6.  geometry primitives: aabb, triangle, segment, ray, line, plane + object_traits   [in progress]  types done; queries planned
7.  geometry measure/query basics      [planned]
8.  curves                             [planned]
9.  symbolic scalars                   [planned]
10. calculus                           [planned]
11. sampling                           [planned]
12. spatial                            [planned]
13. mesh core                          [planned]
14. mesh algorithms                    [planned]
```
