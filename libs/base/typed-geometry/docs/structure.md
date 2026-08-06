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
  linalg/      [done for now]
  transform/   [done]
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

`geometry` depends on `transform`, not the other way round and not as a sibling: a geometric object carries a `.transformed(t)` member, so its header needs the transform type.
Nothing in `transform` names a geometric type — each object writes its own `transformed` in `geometry/`, so the direction stays one-way.

`pos`, `vec` and `bivec` carry a `.transformed(t)` member too, and branch on the transform's class exactly as the geometric objects do — so a pure translation moves a point by one addition and leaves a displacement or a bivector untouched, with no linear part consulted.
That needs the class names, so the linalg headers include `transform/fwd.hh`. That is forward declarations only: `transform/fwd.hh` pulls in nothing from linalg, and the complete transform type is only required at the call site, so the module dependency stays one-way.

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
  mat.hh           [done]     col-major, zero/identity, rotations, products, transposed/determinant/adjugate/cofactor/inverse
  quat.hh          [done]     zero/identity, rotations, products, axis()/angle()
  linalg.hh        [done]
  all.hh           [done]

  norm.hh          [planned]
  normalize.hh     [planned]
  decomposition.hh [planned]
```

`determinant`, `adjugate`, `cofactor` and `inverse` are written out per dimension and stop at 4x4 today.
That is the state of the implementation, not a scope decision: `N > 4` may well land, and until it does a larger matrix `static_assert`s rather than silently misbehaving.
`cofactor` — `m.determinant() * m.inverse().transposed()`, without the division — is what a normal transforms by; `adjugate` is its transpose and stays defined for singular matrices.
`inverse` returns the zero matrix for a singular input instead of asserting — degenerate data is common enough that failing on it is the worse default.

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

## transform/ [done]

Semantic transformation types.

```txt
transform/
  transform_flags.hh              [done]  the capability lattice, all of it in tg::impl
  homogeneous_transform.hh        [done]  the one transform type, <DSource, DTarget, T, Flags>; representation chosen from the flags, plus composed / inverse / transform / operator()
  impl/rotation_representation.hh [done]  2D unit complex / 3D quat, identity by default
  impl/transform_representation.hh [done] the flags -> layout table
  composed_transform.hh           [done]  two arbitrary transforms held side by side, applied in order
  compose.hh                      [done]  tg::compose(a, b): fuse via composed() if possible, else nest
  fwd.hh, transform.hh, all.hh    [done]
```

There is **one** transform type, `homogeneous_transform<DSource, DTarget, T, Flags>`, and the named classes are aliases over it — all of them square:

```cpp
tg::rigid_transform3f       // rotation | translation
tg::similarity_transform3f  // + a uniform scale
tg::affine_transform3f      // a general linear part | translation
tg::projective_transform3f  // + a non-trivial homogeneous row
```

`Flags` selects the representation, which is a private member — a rigid transform is a quat plus a vec, an affine one a mat plus a vec, a pure translation just a vec.
`tg::impl::transform_representation_of(t)` is the one way to it, for an object that can beat the public accessors by reading the members directly.
Default construction is the identity, not a zero-filled value.

The **source and target dimensions** are what will make lifting and projecting typed: a `pos<DSource, T>` goes in, a `pos<DTarget, T>` comes out, the linear part is a `mat<DSource, DTarget, T>` and the translation lives in the target space.
`t.inverse()` swaps the pair, and `composed` chains it.
Only the square case is implemented — the type `static_assert`s `DSource == DTarget` — so the parameter is in place and the mixed-dimension maths is not.

### The flag lattice

Several bit patterns denote the same set of transforms, so `tg::impl::transform_canonical` reduces each to one representative; there are exactly **13** classes.
The whole lattice lives in `tg::impl`: it is machinery, and a class is named through its alias.
Two of its rules are worth knowing:

- rotation together with non-uniform scaling is a **general linear map**. `R1 S1 R2 S2` is not of the form `R S`, and by the SVD the closure really is all of `GL(D)` — so the class widens rather than pretending to stay separable.
- scale factors are **positive** unless the class carries `negative_scaling`. That keeps the narrow classes simple: under a positive scaling an aabb's corners stay in order and a sphere's radius is just multiplied.
  The `signed_*` classes opt in, and the factories assert the promise.
  In 3D `signed_similarity` is the full conformal group, since a negative uniform scale composed with a half-turn is a plane reflection — which is why a sphere still maps to a sphere under it.

**Containment is `tg::impl::transform_is_subclass`, never `has_all`.** Canonicalization *clears* bits — affine drops `uniform_scaling` because `non_uniform_scaling` subsumes it — so `has_all(affine, similarity)` is false even though every similarity is affine.
`transform_canonical(a | b)` is the join, so containment is "the join is already the wider class".

Widening between classes is lossless but **explicit**, and narrowing is not a constructor at all.
That widening constructor is also the dispatch mechanism — see the object handshake below.

### Applying a transform

```cpp
p.transformed(t)        // pos, incl. the projective divide
v.transformed(t)        // vec — not available for a projective transform
b.transformed(t)        // bivec, by the second exterior power
obj.transformed(t)      // geometric objects
t.transform(obj)        // the mirror spelling; t(obj) is the call spelling of it

a.composed(b)           // applies b FIRST, fusing into one transform of the join class. NO operator*
tg::compose(a, b)       // a.composed(b) where that exists, else a composed_transform holding both
t.inverse()             // every canonical class is closed under it, so the class is unchanged
t.to_mat()              // the homogeneous matrix
```

Applying and composing are one operation:

```cpp
a(b(obj))  ==  a.composed(b).transform(obj)  ==  obj.transformed(b).transformed(a)
```

Composing is **opt-in** — a transform declares `composed` for the transforms it can absorb, and unlike `transformed` that is probeable with `requires { a.composed(b); }`.
`tg::compose` is total anyway: it fuses through `composed` when it can and otherwise returns a `composed_transform`, which stores both and applies the inner one first.
The choice is `if constexpr`, so the return type tells you which one you got.
There is no `operator*`: a transform is applied, not multiplied, and `*` would invite a `t * p` that tg deliberately does not have.

A bivector does **not** transform by the linear part — it transforms by that map's second exterior power, so in 3D its dual (a normal) picks up the cofactor matrix.
That is the whole reason a normal must be a `bivec` and not a `vec`.

`mat` stays a linear-algebra object: there is no `m * p`, and no `t * p` either.

### The object handshake

An object's `transformed` member asks the transform, in the object's own order of preference, which **view** it can produce.
The first branch that compiles decides both the maths and the result type:

```cpp
template <class TransformT>
[[nodiscard]] constexpr auto transformed(TransformT const& t) const
{
    if constexpr (requires { t.custom_transform(*this); })   // the transform special-cases this object
        return t.custom_transform(*this);
    else if constexpr (requires { tg::similarity_transform<DAmbient, T>(t); })   // angles preserved -> still a sphere
        ...
    else if constexpr (requires { tg::affine_transform<DAmbient, T>(t); })       // ... otherwise an ellipsoid
        ...
    else
        static_assert(false, "...");
}
```

The priority order is literally the order of the branches, written in the object's own header next to the maths it selects.
The library side is nothing beyond the widening constructor: it is gated on `is_subclass`, so `tg::similarity_transform<D, T>(t)` compiles exactly when `t` really is a similarity.
`obj.transformed(t)`, `t.transform(obj)` and `t(obj)` are all public and mean the same thing — the last two route straight back to the first, so only the object ever decides.
A transform special-cases an object with a **private** `custom_transform(ObjT const&)` plus a friend declaration for it, which is the first branch every object checks; access checking is part of that `requires`, so a non-befriended object never sees the branch.

One branch covers many inputs: `aabb` asks only for `scaling_translation_transform<D, T>` and thereby handles the identity, a pure translation and both scalings.

An unsupported pair is a **compile error**, deliberately — a rotated `aabb` is not an `aabb`, and returning an enlarged one silently would be worse.
Since the member's return type is `auto`, asking "is this supported?" would instantiate the body and trip the `static_assert`; probe the branch condition (`requires { tg::affine_transform<D, T>(t); }`) instead.

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
    sphere.hh       [done]     sphere surface {x : distance(x, center) == radius}
    ellipsoid.hh    [done]     ellipsoid surface {center + sum_i u_i * semi_axes[i] : |u| == 1}
    primitives.hh   [done]
    # planned: obb, ball, quadric, polygon, ...
  query/            [planned]  # distance, projection, closest, intersection, intersects, containment, ...
  measure/          [planned]  # area, volume, centroid, bounds, moments
  construct/        [planned]  # hull, fitting, primitives_from_points
  geometry.hh       [done]
  all.hh            [done]
```

Every primitive denotes a **set of points**, classified by an `object_traits` specialization colocated with the type.
[modules/geometry.md](modules/geometry.md) covers what those facts mean, why representation is not interpretation, and why `sphere`/`ellipsoid` carry an embedding dimension.

Each primitive also registers what it becomes under a transform.
[modules/transform.md](modules/transform.md) carries the registration table and why its gaps are missing types rather than skipped work.

Queries are intentionally **not** implemented yet — the representations settle first.
When they land, members stay intrinsic and cheap (`ray.at(t)`, `aabb.center()`, `triangle.area()`).
Symmetric or cross-type queries are free functions: `distance(a, b)`, `intersection(a, b)`.
[plans/geometry-query-matrix.md](plans/geometry-query-matrix.md) is the agreed shape for that layer.

## curves/ [planned]

Parametric curves and surfaces (`curve`, `surface`, `bezier`, `spline`, `nurbs`, `polycurve`; `evaluation`, `derivatives`, `fitting`, `subdivision`).

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

Basic CAS and exact symbolic algebra (`bigint`, `bigrat`, `monomial`, `polynomial`, `rational_polynomial`, `expr`; `simplify`, `factor`, `substitute`, `derivative`, `evaluate`).

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

Sampling algorithms and distributions (`random`, `distribution`, `low_discrepancy`, `blue_noise`; domain samplers for sphere/hemisphere/disk/triangle/polygon/mesh).

```cpp
sample_sphere(rng)
sample_triangle(tri, rng)
sample_mesh(mesh, rng)
```

## spatial/ [planned]

Acceleration structures and space partitioning (`bvh`, `kd_tree`, `grid`, `hash_grid`, `octree`; `builder`, `queries`, `traversal`).

```cpp
bvh<Triangle> b;
b.intersect(ray)
b.closest(p)
```

## mesh/ [planned]

Mesh data structures and mesh-domain algorithms (`core/`, `polygon/`, `triangle/`, `halfedge/`, `attributes/`, `algorithms/`, `io/`). Mesh-specific algorithms (triangulate, rasterize, remesh, simplify, repair, weld, smooth, subdivide, boolean support, parameterize) live here, not at the top level.

## Umbrella Include Policy

Each module provides a curated common include (`module/module.hh`) and a complete, potentially expensive one (`module/all.hh`).
The top-level `<typed-geometry/all.hh>` pulls in everything.

```cpp
#include <typed-geometry/linalg/linalg.hh>       // the common linalg types
#include <typed-geometry/transform/transform.hh> // the transform type and its operations
#include <typed-geometry/linalg/all.hh>          // everything in linalg
#include <typed-geometry/all.hh>                 // everything (expensive)
```

## Initial Implementation Order

```txt
1.  scalar traits/constants            [in progress]  traits, constants, one/sqrt/sin/cos, angle done
2.  linalg: vec, pos, comp             [done]
3.  linalg: bivec + cross/dual/undual  [done]
4.  linalg: mat, quat                  [done]
5.  transform: the flag lattice, homogeneous_transform, transformed(pos/vec/bivec), the object handshake   [done]
6.  geometry primitives: aabb, triangle, segment, ray, line, plane, sphere, ellipsoid + object_traits   [in progress]  types done; queries planned
7.  geometry measure/query basics      [planned]
8.  curves                             [planned]
9.  symbolic scalars                   [planned]
10. calculus                           [planned]
11. sampling                           [planned]
12. spatial                            [planned]
13. mesh core                          [planned]
14. mesh algorithms                    [planned]
```
