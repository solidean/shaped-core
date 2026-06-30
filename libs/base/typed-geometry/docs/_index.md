# typed-geometry docs

Documentation hub for typed-geometry. For the library overview, types, and how to include
headers, start at the [readme](../readme.md). For repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

## Source organization

typed-geometry's headers live in `src/typed-geometry/`, grouped by module. `scalar/`, `linalg/`,
and the first `geometry/` primitives exist today; the rest of the modules in the roadmap are
planned.

```text
typed-geometry/
  fwd.hh        # aggregate forward decls (just includes the per-module <module>/fwd.hh)
  all.hh        # complete umbrella (expensive)
  scalar/       # scalar_traits<T> seam + tg::sqrt (angle/complex/interval/... planned)
  linalg/       # vec, pos, comp, bivec, mat, quat (+ ops)
  geometry/     # object_traits seam + primitives (aabb/triangle/segment/ray/line/plane); queries planned
```

Forward declarations and the dimensional/typed aliases live in a per-module `fwd.hh`
(`scalar/fwd.hh`, `linalg/fwd.hh`, `geometry/fwd.hh`), chained along the dependency order; the
top-level `fwd.hh` just aggregates them. Include a single module's `fwd.hh` when that is all you
need.

## Module docs

One doc per module under [modules/](modules/), answering **"what belongs here?"** and **"why is it
this way?"** — motivation, scope, and the load-bearing design decisions (the kind that trip people
up), in the spirit of an ADR. Not a cheat sheet, not the roadmap. Add one when a module lands; cover
the big rationales (small ones stay in source comments).

- [modules/scalar](modules/scalar.md) — the scalar seam, `angle`, which types count as scalars.
- [modules/linalg](modules/linalg.md) — `vec`/`pos`/`comp`/`bivec`/`mat`/`quat`; the `pos + pos`
  translation rule and the `bivec != vec` decision.
- [modules/geometry](modules/geometry.md) — the primitive types and the `object_traits`
  point-set seam (`intrinsic_dim`/`ambient_dim`/`is_finite`); representation vs. interpretation.

## Topics

- [structure](structure.md) — the full module roadmap with per-section `[done]`/`[in
  progress]`/`[planned]` status. This is the living design document; update it as modules land.
- [TODO](TODO.md) — running list of known follow-ups.
- [coding-guidelines](coding-guidelines.md) — tg-specific conventions on top of the repo-wide
  ones; extend it whenever generic advice turns out not to fit tg.

## Conventions

- Namespace `tg`; depends on clean-core (vocabulary types + assertions).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md) plus the
  tg-specific [coding-guidelines](coding-guidelines.md) (scalar traits, raw `data` storage,
  generic-over-`D` types, …). `.clang-format` is authoritative for formatting.
