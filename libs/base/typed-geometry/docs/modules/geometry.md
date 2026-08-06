# Module: geometry

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`geometry/` holds the geometric primitive *types* — `aabb`, `triangle`, `segment`, `ray`, `line`, `plane` — and the `object_traits` seam that classifies them.
It depends on `linalg/`, since the primitives are phrased in `pos`/`vec`, and on `scalar/`.
Geometric *queries* (containment, distance, closest point, intersection) and *measures* (area, centroid, bounds) are planned siblings: `geometry/query/`, `geometry/measure/`, ….
They deliberately do **not** exist yet — the representations get to settle first.
[plans/geometry-query-matrix.md](../plans/geometry-query-matrix.md) is the agreed shape of that query layer.

## What belongs here

- Primitive **data** types under `primitives/`, each its own header, with cheap intrinsic members
  only (storage, construction, equality). No pairwise queries on the types.
- The `object_traits<ObjT>` seam (`traits.hh`) and its `tg::traits::*` helpers.
- Later: `query/`, `measure/`, `construct/` as separate subfolders (free functions over the
  primitives).

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
`sphere` and the planned `ball` are the same pairing — `sphere` is the surface, so its `intrinsic_dim` is `D - 1`.

### `sphere` and `ellipsoid` carry an embedding dimension

Both take **two** dimensions, `<D, DAmbient, T>`: the flat the object curves in, and the space that flat sits in.
They coincide for the everyday cases (`sphere3f`, `ellipsoid2f`) and part when the object is embedded above its own dimension — `sphere2in3f` is a circle lying in 3D, `ellipsoid2in3f` an ellipse.

An `ellipsoid`'s semi-axes span its flat, so one general template covers every pair and the embedded case stores nothing extra.
A `sphere`'s `{center, radius}` does not say which plane the circle lies in, so what it stores depends on the pair.
Its primary template is therefore left undefined and each supported pair is a specialization: `sphere<D, D, T>` is `{center, radius}`, and `sphere<2, 3, T>` adds the plane's normal.
A pair with no specialization is an incomplete type, which is the same "opt in per case, never a silent default" stance `object_traits` takes.

### Minimal surface, no queries yet

The primitives carry only storage, constructors and a defaulted `operator==`.
No `contains`, `distance`, `at(t)`, area or `make_*` factories: those are deferred until the representations have settled, so no API gets baked in that we would then rework.
Named vertex members (`pos0`/`pos1`/`pos2`) are used instead of the `data[]`/`operator[]` storage of the linalg types.
A triangle's vertices are distinct sub-objects rather than interchangeable components, so the linalg "no `.x/.y`, index only" rule does not apply here.

## See also

- [structure.md](../structure.md) — the roadmap; `geometry/` is item 6.
- [modules/linalg](linalg.md) — the `pos`/`vec` types the primitives are built from.
- [cheat-sheet](../../cheat-sheet.md) — the geometry API at a glance.
- source: [traits.hh](../../src/typed-geometry/geometry/traits.hh),
  [primitives/](../../src/typed-geometry/geometry/primitives/).
