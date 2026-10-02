# Module: geometry

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`geometry/` holds the geometric primitive *types* and the `object_traits` seam that classifies them.
The types are point sets of every kind the old tg had: linear objects, flats, polytopes such as `box`, round objects such as `capsule`, their unbounded forms, and the frustum.
Each solid has its boundary as a type of its own.
[plans/old-tg-carryover.md](../plans/old-tg-carryover.md) is the roster.
It depends on `linalg/`, since the primitives are phrased in `pos`/`vec`, and on `scalar/`.
Geometric *queries* (containment, distance, closest point, intersection) are member functions whose definitions live in `geometry/query/`, one header per verb.
[plans/geometry-query-matrix.md](../plans/geometry-query-matrix.md) is that layer, and [plans/old-tg-carryover.md](../plans/old-tg-carryover.md) the object roster and the per-type verbs.

## What belongs here

- Primitive **data** types under `primitives/`, each its own header: storage, construction, equality, the per-type unary members, and the declarations of the query members.
- The `object_traits<ObjT>` seam (`traits.hh`) and its `tg::traits::*` helpers.
- `query/`: the definitions of the query members, plus GJK and EPA.
- Later: `construct/` (hulls, fitting).

## What does NOT belong here

- Acceleration structures (bvh, grids) — those are the planned `spatial/` module.
- Transforms — a `mat`/`rigid_transform` is `linalg/` / `transform/`, not geometry.

## Key decisions

### Every object is a *set of points*

The whole module is organized around one idea: a geometric object **is** the set of points it represents.
Every future query — `contains`, `distance`, `intersection` — is phrased against that set, and the struct is just an encoding of it.
That keeps the query layer uniform: it never special-cases what a type *means*, only "is this point in the set" and "how far to the set".

Each primitive's `///` states its set exactly.
The ones worth internalizing:

- `aabb` — the **solid** box `{x : min <= x <= max}` (not just its faces).
- `triangle` — the **filled** triangle (convex hull of the three vertices), a 2D patch.
- `segment` — `{(1-t)·pos0 + t·pos1 : t in [0,1]}`, endpoints included.
- `ray` — `{origin + t·dir : t >= 0}`; `line` — the same with `t in R`.
- `plane` — the points **on** the hyperplane `{x : dot(normal, x) == dist}`, *not* a half-space.
- `halfspace` — `{x : dot(normal, x) <= dist}`, plane's encoding, with the plane as its boundary.
- `box` — `{center + H·c : c in [-1, 1]^D}`, the half-axes as `H`'s columns and not necessarily orthogonal, so every affine image of a box is a box.

### `object_traits`: `intrinsic_dim`, `ambient_dim`, `is_finite`

`object_traits<ObjT>` (in [traits.hh](../../src/typed-geometry/geometry/traits.hh)) records three
facts about the point set, mirroring the `scalar_traits` pattern — a primary template each type
specializes **in its own header**, read through `tg::traits::intrinsic_dim/ambient_dim/is_finite`:

- **`ambient_dim`** — the dimension of the space the points live in.
- **`intrinsic_dim`** — the dimension of the set itself as a manifold.
  The two differ: a triangle with 3D coordinates is a 2D object in a 3D world, so `intrinsic_dim == 2` and `ambient_dim == 3`.
  Always `intrinsic_dim <= ambient_dim`, and a hyperplane is codimension 1 (`intrinsic_dim == ambient_dim - 1`).
- **`is_finite`** — whether the set is bounded.
  `aabb`/`triangle`/`segment` are finite; `ray`/`line`/`plane` are not.

The primary template is intentionally left **undefined**, so a type that forgets to specialize it is
a hard compile error rather than getting silent wrong defaults.

### Representation is not interpretation

Two objects can share an encoding yet denote different sets.
`plane` stores `{normal, dist}` and denotes the points *on* the plane.
The planned `halfspace` will reuse the **exact same** `{normal, dist}` representation but denote `{x : dot(normal, x) <= dist}`, one side of the plane.
The point-set framing is what makes that distinction explicit instead of accidental.
So the interpretation lives in the type and its `object_traits`, never implicitly in the storage.
The boundary types are the same pairing: `sphere` and `sphere_boundary` share `{center, radius}` and denote the ball and its surface.

### Maximal by default, boundaries as types of their own

An object type denotes the **largest** set its representation can mean: `aabb3` is the solid box, `sphere3` the ball.
Its boundary is a separate struct per family — `aabb_boundary<D, T>`, `sphere_boundary<D, DAmbient, T>`, with `aabb3_surface`-style typedefs in 3D — and `.boundary()` / `.solid()` convert explicitly.
A type in a diagnostic is then exactly the set its doc states, and passing a surface where a solid was meant does not compile.
A *mantle* is a boundary without its flat caps (`cylinder_mantle`), and is a type of its own too.

### `sphere` and `ellipsoid` carry an embedding dimension

Both take **two** dimensions, `<D, DAmbient, T>`: the flat the object curves in, and the space that flat sits in.
They coincide for the everyday cases (`sphere3f`, `ellipsoid2f`) and part when the object is embedded above its own dimension.
`sphere2in3f` (`disk3f`) is a disk lying in 3D, `ellipsoid2in3f` a filled ellipse.

An `ellipsoid`'s semi-axes span its flat, so one general template covers every pair and the embedded case stores nothing extra.
A `sphere`'s `{center, radius}` does not say which plane the circle lies in, so what it stores depends on the pair.
Its primary template is therefore left undefined and each supported pair is a specialization: `sphere<D, D, T>` is `{center, radius}`, and `sphere<2, 3, T>` adds the plane's normal.
A pair with no specialization is an incomplete type, which is the same "opt in per case, never a silent default" stance `object_traits` takes.

### Named vertex members

Named vertex members (`pos0`/`pos1`/`pos2`) are used instead of the `data[]`/`operator[]` storage of the linalg types.
A triangle's vertices are distinct sub-objects rather than interchangeable components, so the linalg "no `.x/.y`, index only" rule does not apply here.

## See also

- [structure.md](../structure.md) — the roadmap; `geometry/` is item 6.
- [modules/linalg](linalg.md) — the `pos`/`vec` types the primitives are built from.
- [cheat-sheet](../../cheat-sheet.md) — the geometry API at a glance.
- source: [traits.hh](../../src/typed-geometry/geometry/traits.hh),
  [primitives/](../../src/typed-geometry/geometry/primitives/).
