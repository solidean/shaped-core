# typed-geometry structure proposal (tg::)

This is the living roadmap for typed-geometry.
Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started

Update the tags as modules land.
This document is design intent, not a guarantee of final API.

> **Layout deviation from the original proposal:** headers live in `src/typed-geometry/` (the shaped-core convention — `.hh`/`.cc` colocated under `src/<lib>/`), **not** in `include/typed-geometry/`. Include paths are unchanged: `#include <typed-geometry/...>`.

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

## Representation note (current implementation)

The implemented `comp` / `vec` / `pos` are a single generic type each (`vec<int D, class T>`), not per-dimension specializations.
Typedefs exist for D = 2/3/4 (`vec2f`, `vec3f`, `vec4f`, … suffixes `f`=f32, `d`=f64, `i`=i32), but the types stay generic over `D`.

Storage is a public raw C array member named `data` (`T data[D]`). **There are no `.x/.y/.z` members**, not even as accessor functions — components are reached via `data` or `operator[]`. Default construction zero-initializes.
Dimension-specific behavior is gated with `requires`. We are deliberately seeing how far this minimal representation carries us.

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

`scalar_traits<T>` is the extensibility seam: every scalar capability (`has_sqrt`, `has_trigonometry`, `one`, `is_zero`, `is_one`, `sqrt`, `sin`, `cos`, `atan2`) routes through it, so custom scalar types (expression trees, double-double, bigint/bigrat) can opt in by specializing the trait — `is_zero`/`is_one` in particular let symbolic/exact scalars give a smarter answer than a raw comparison.
tg avoids `std::` type-traits / `<cmath>` in user-facing code for this reason.
`f32`/`f64` are fully featured; every integer type (incl. `signed`/`unsigned char` but **not** plain `char`) gets `one`/`is_zero`/`is_one`; `bool` has its own specialization.
`bigint`/`bigrat` are expected to live here too (useful as scalars outside symbolic algebra).

`angle<T>` stores radians, is built only via `make_from_radians`/`make_from_degree`, read via `.radians()`/`.degree()`, and has `_rad_f`/`_rad_d`/`_deg_f`/`_deg_d` literals (e.g. `90_deg_f`). It supports addition and scalar multiplication with no wrap-around.

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
  mat_ops.hh       [done]     transpose, determinant, adjugate, cofactor, inverse (N <= 4)
  quat.hh          [done]     zero/identity, rotations, products, axis()/angle()
  linalg.hh        [done]
  all.hh           [done]

  norm.hh          [planned]
  normalize.hh     [planned]
  decomposition.hh [planned]
```

`mat_ops` stops at 4x4 because tg is 2D/3D: the linear part tops out at `mat3` and the homogeneous matrix at `mat4`, so there is no general-N path to write.
`cofactor` — `determinant(m) * transpose(inverse(m))`, without the division — is what a normal transforms by; `adjugate` is its transpose and stays defined for singular matrices.
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

`mat` is not a transform type.
It is linear algebra data.

Special values are exposed as static constants (`vec::zero`, `pos::zero`, `comp::zero`, `bivec::zero`, `mat::zero`, `mat::identity`, `quat::zero`, `quat::identity`). They are runtime constants (a static member can't be `constexpr` of its own incomplete type), not constant expressions.
All factory methods are named `make_*` (e.g. `vec::make_unit`, `make_from_values`, `mat::make_rotation_z`).

`comp` is the "semantics-free" building block in the sense that it is the raw component-wise type.
It is therefore the home of **all** component-wise arithmetic: every operator is element-wise (including Hadamard `*`/`/`) and a scalar operand broadcasts, plus component-wise `min`/`max`. `vec` and `pos` deliberately omit these — they only make sense on plain components.

`pos` is special: it is both a linalg and a geometric object.
Geometric objects are sets of points, and `pos` is a singleton set — so `pos + pos -> pos` because adding translates the `{pos}` object.
We treat `+ vec` and `+ pos` as applying a translation.

Matrices will be **column-major** stored, with C++23 multi-argument `operator[]` for element access (lands with `mat`).

## transform/ [done]

Semantic transformation types.

```txt
transform/
  transform_flags.hh          [done]  the capability lattice: canonical(), is_subclass(), transform_class::*
  homogeneous_transform.hh    [done]  the one transform type, <DSource, DTarget, T, Flags>; storage chosen from the flags, plus composed / transform / operator()
  impl/rotation_storage.hh    [done]  2D unit complex / 3D quat, identity by default
  impl/transform_storage.hh   [done]  the flags -> layout table
  composed_transform.hh       [done]  two arbitrary transforms held side by side, applied in order
  compose.hh                  [done]  tg::compose(a, b): fuse via composed() if possible, else nest
  inverse.hh                  [done]  tg::inverse(t)
  fwd.hh, transform.hh, all.hh  [done]
```

There is **one** transform type, `homogeneous_transform<DSource, DTarget, T, Flags>`, and the named classes are aliases over it — all of them square:

```cpp
tg::rigid_transform3f       // rotation | translation
tg::similarity_transform3f  // + a uniform scale
tg::affine_transform3f      // a general linear part | translation
tg::projective_transform3f  // + a non-trivial homogeneous row
```

`Flags` selects the storage and is never exposed — a rigid transform is a quat plus a vec, an affine one a mat plus a vec, a pure translation just a vec.
Default construction is the identity, not a zero-filled value.

The **source and target dimensions** are what will make lifting and projecting typed: a `pos<DSource, T>` goes in, a `pos<DTarget, T>` comes out, the linear part is a `mat<DSource, DTarget, T>` and the translation lives in the target space.
`tg::inverse` swaps the pair, and `composed` chains it.
Only the square case is implemented — the type `static_assert`s `DSource == DTarget` — so the parameter is in place and the mixed-dimension maths is not.

### The flag lattice

Several bit patterns denote the same set of transforms, so `tg::canonical` reduces each to one representative; there are exactly **13** classes.
Two of its rules are worth knowing:

- rotation together with non-uniform scaling is a **general linear map**. `R1 S1 R2 S2` is not of the form `R S`, and by the SVD the closure really is all of `GL(D)` — so the class widens rather than pretending to stay separable.
- scale factors are **positive** unless the class carries `negative_scaling`. That keeps the narrow classes simple: under a positive scaling an aabb's corners stay in order and a sphere's radius is just multiplied.
  The `signed_*` classes opt in, and the factories assert the promise.
  In 3D `signed_similarity` is the full conformal group, since a negative uniform scale composed with a half-turn is a plane reflection — which is why a sphere still maps to a sphere under it.

**Containment is `tg::is_subclass`, never `has_all`.** Canonicalization *clears* bits — affine drops `uniform_scaling` because `non_uniform_scaling` subsumes it — so `has_all(affine, similarity)` is false even though every similarity is affine.
`canonical(a | b)` is the join, so containment is "the join is already the wider class".

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
tg::inverse(t)          // every canonical class is closed under it, so the class is unchanged
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
Primitive *types* and the `object_traits` seam have landed; queries/measures/construction are still planned.

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

Every primitive denotes a **set of points** and is classified by `object_traits` (specialized in its own header, colocated with the type): `intrinsic_dim` (the object's manifold dimension — a 3D triangle is a 2D object, so 2), `ambient_dim` (the surrounding space — 3 for that triangle), and `is_finite` (triangle/segment/aabb yes; ray/line/plane no). Representation is not interpretation: `plane` and the planned `halfspace` will share the `{normal, dist}` encoding but denote the points *on* vs. *on one side of* the hyperplane.
`sphere` and the planned `ball` are the same pairing — `sphere` is the surface, so its `intrinsic_dim` is `D - 1`. See [modules/geometry.md](modules/geometry.md).

`sphere` and `ellipsoid` take **two** dimensions, `<D, DAmbient, T>`: the flat the object curves in, and the space that flat sits in.
They coincide for the everyday cases (`sphere3f`, `ellipsoid2f`) and part when the object is embedded above its own dimension — `sphere2in3f` is a circle lying in 3D, `ellipsoid2in3f` an ellipse.
An `ellipsoid`'s semi-axes span its flat, so one general template covers every pair — the embedded case stores nothing extra.
A `sphere`'s `{center, radius}` does not say which plane the circle lies in, so what it stores depends on the pair: its primary template is left undefined and each supported pair is a specialization (`sphere<D, D, T>` is `{center, radius}`, `sphere<2, 3, T>` adds the plane's normal).
A pair with no specialization is an incomplete type, which is the same "opt in per case, never a silent default" stance `object_traits` takes.

Each primitive also registers what it becomes under a transform; see the `transform/` section above.
Which registrations exist is a statement about geometry, not about effort:

| primitive | registered at | result |
|---|---|---|
| `sphere` | similarity / affine (unless embedded) | `sphere` / **`ellipsoid`** |
| `ellipsoid` | affine | `ellipsoid` |
| `aabb` | scaling + translation **only** | `aabb` |
| `triangle`, `segment` | affine, projective | unchanged |
| `plane` | affine, projective | `plane` |
| `ray`, `line` | affine **only** | unchanged |

The gaps are where a type is missing, not where work was skipped.
A rotated `aabb` is an oriented box (`obb`); a projected `ray` is a bounded segment, because its point at infinity maps to a finite point; a projected `sphere` is a general quadric.
The affine image of an *embedded* `sphere` is the one gap left in that table: it is an ellipse in the ambient space, but naming it needs an orthonormal basis of the circle's plane, which `linalg` has no routine for yet.
Each of those is a compile error until the type — or the routine — that would hold the answer exists.

A finite convex primitive given by its vertices does survive a projection: `w` is affine over the primitive and the positive-`w` halfspace is convex, so asserting `w > 0` at the vertices settles the whole hull.
An unbounded primitive generally does not.

Queries are intentionally **not** implemented yet — the representations settle first.
When they land, member functions stay intrinsic/cheap (e.g. `ray.at(t)`, `aabb.center()`, `aabb.contains(p)`, `triangle.area()`), while symmetric/cross-type queries are free functions (`distance(a, b)`, `closest_point(p, primitive)`, `intersects(a, b)`, `intersection(a, b)`, `contains(a, b)`). Avoid making every pairwise query a member.

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
  Geometry may instantiate over symbolic scalars, but symbolic should not know geometry.
- `bigint`/`bigrat` likely belong to `scalar/` because they are useful as scalars outside of symbolics.

## calculus/ [planned]

Differentiation, integration, and optimization (`autodiff`, `fwd_diff`, `rev_diff`, `integrate`, `optimize`, `root_find`, `minimization`). Thin wrappers may re-export scalar AD types.
Heavy optimization/integration algorithms live here, not in a top-level `algo/`.

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

Each module provides a curated common include (`module/module.hh`) and a complete, potentially expensive one (`module/all.hh`). The top-level `<typed-geometry/all.hh>` pulls in everything.

```cpp
#include <typed-geometry/linalg/linalg.hh>     // vec/pos/comp/bivec/mat/quat and their operations
#include <typed-geometry/transform/transform.hh> // the transform type and its operations
#include <typed-geometry/linalg/all.hh>        // everything in linalg
#include <typed-geometry/all.hh>               // everything (expensive)
```

## Initial Implementation Order

```txt
1.  scalar traits/constants            [in progress]  traits, constants, one/sqrt/sin/cos, angle done
2.  linalg: vec, pos, comp             [done]         (comp arithmetic still planned)
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
