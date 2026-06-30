# Module: geometry

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — the load-bearing
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)) and not the roadmap
> (that's [structure.md](../structure.md)). They capture the rationale behind choices that would
> otherwise trip up a reader.

## What this module is

`geometry/` holds geometric primitive *types* (`aabb`, `triangle`, `segment`, `ray`, `line`,
`plane`) and the `object_traits` seam that classifies them. It depends on `linalg/` (the primitives
are phrased in `pos`/`vec`) and `scalar/`. Geometric *queries* (containment, distance, closest
point, intersection) and *measures* (area, centroid, bounds) are planned siblings —
`geometry/query/`, `geometry/measure/`, … — and deliberately do **not** exist yet: we want to settle
the representations first.

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

The whole module is organized around one idea: a geometric object **is** the set of points it
represents, and every future query is phrased against that set (`contains`, `distance`,
`intersection`, …). The struct is just an encoding of that set. This keeps the query layer uniform:
it never has to special-case "what does this type mean", only "is this point in the set", "how far
to the set", etc.

Each primitive's `///` states its set exactly. The ones worth internalizing:

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
- **`intrinsic_dim`** — the dimension of the set itself as a manifold. These differ: a triangle with
  3D coordinates is a 2D object in a 3D world, so `intrinsic_dim == 2`, `ambient_dim == 3`. Always
  `intrinsic_dim <= ambient_dim`; a hyperplane is codimension 1 (`intrinsic_dim == ambient_dim - 1`).
- **`is_finite`** — whether the set is bounded. `aabb`/`triangle`/`segment` are finite;
  `ray`/`line`/`plane` are not.

The primary template is intentionally left **undefined**, so a type that forgets to specialize it is
a hard compile error rather than getting silent wrong defaults.

### Representation is not interpretation

Two objects can share an encoding yet denote different sets. `plane` stores `{normal, dist}` and
denotes the points *on* the plane. The planned `halfspace` will reuse the **exact same**
`{normal, dist}` representation but denote `{x : dot(normal, x) <= dist}` — one side of the plane.
The trait/point-set framing is what makes that distinction explicit instead of accidental, so we
keep the interpretation in the type (and its `object_traits`), never implicit in the storage.

### Minimal surface, no queries yet

The primitives carry only storage, constructors, and a defaulted `operator==`. No `contains`,
`distance`, `at(t)`, area, or `make_*` factories — those are intentionally deferred until the
representations have settled, to avoid baking in an API we will rework. Members like named vertices
(`pos0`/`pos1`/`pos2`) are used instead of the `data[]`/`operator[]` storage of the linalg types: a
triangle's vertices are distinct sub-objects, not interchangeable components, so the linalg
"no `.x/.y`, index only" rule does not apply here.

## See also

- [structure.md](../structure.md) — the roadmap; `geometry/` is item 6.
- [modules/linalg](linalg.md) — the `pos`/`vec` types the primitives are built from.
- [cheat-sheet](../../cheat-sheet.md) — the geometry API at a glance.
- source: [traits.hh](../../src/typed-geometry/geometry/traits.hh),
  [primitives/](../../src/typed-geometry/geometry/primitives/).
