# typed-geometry

Strongly-typed C++23 math & geometry library.
Namespace `tg`. Depends on **clean-core** (for the vocabulary types `i32`/`f32`/`isize`/… and assertions).

```cpp
#include <typed-geometry/linalg/linalg.hh>

auto const a = tg::pos3f(1, 2, 3);
auto const b = tg::pos3f(4, 6, 3);
tg::vec3f const d = b - a;          // displacement between points
auto const dist = d.length();       // 5
```

Headers are included by their full path from `src/`, e.g. `#include <typed-geometry/linalg/vec.hh>`. `fwd.hh` (at the root) forward-declares the public types and defines the dimensional/typed aliases.

This library is at an **early stage** — the `scalar/` trait seam, the `linalg/` core, the `transform/` module and the first `geometry/` primitives exist; queries, curves, meshes and the rest do not.
See [docs/structure.md](docs/structure.md) for the full roadmap and what is `[done]` vs `[planned]`.

## Design at a glance

- **Semantic types.** `vec` is a displacement/direction, `pos` is a point, `comp` is the neutral component container.
  Their arithmetic reflects affine geometry: `pos - pos -> vec`, `pos + vec -> pos`, `vec + vec -> vec`, and `pos + pos -> pos` (translation of the singleton point set).
- **One generic type per family.** `vec<int D, class T>` (and `pos`, `comp`), with typedefs for D = 2/3/4: `vec2f`/`vec3f`/`vec4f`, `…d` (f64), `…i` (i32). No per-dimension specializations.
- **Raw storage, indexed access only.** Components live in a public C array member `data` (`T data[D]`). There are **no `.x/.y/.z`** members — use `data` or `operator[]`. Default construction zero-initializes.
  Dimension-specific behavior is gated with `requires`.
- **Extensible scalars.** Scalar capabilities (`sqrt`, trigonometry) route through `tg::scalar_traits<T>`, not `std::` directly, so custom scalar types (expression trees, double-double, bigint, …) can opt in.
  `length()`/`normalized()`/`distance()` are available only for scalars whose trait declares `has_sqrt`.
- **Transforms carry their capabilities in the type.** `tg::rigid_transform3f` and `tg::affine_transform3f` are the same `homogeneous_transform<DSource, DTarget, T, Flags>` at different points of a 19-class lattice; the representation follows from the flags, and so does the result of `obj.transformed(t)` — a sphere stays a sphere under a similarity and becomes an ellipsoid under an affine map.
  The type also names the source and target dimension, so lifting and projecting between spaces can eventually be typed; only the square case is implemented today, and every named alias is square.
  `mat` remains linear-algebra data: there is no `mat * pos`.

## File organization

Source lives in `src/typed-geometry/`, grouped by module:

| Folder       | What's in it |
|--------------|--------------|
| (root)       | `fwd.hh` (forward decls + aliases), `all.hh` (full umbrella) |
| `scalar/`    | `scalar_traits<T>` seam, `tg::sqrt` + trig, `angle<T>` |
| `linalg/`    | `vec`, `pos`, `comp`, `bivec`, `mat`, `quat` and their `_ops` free functions |
| `transform/` | `homogeneous_transform<DSource, DTarget, T, Flags>`, `composed`/`inverse`, and the object handshake |
| `geometry/`  | the `object_traits` seam + the primitives (`aabb`, `triangle`, `sphere`, …) |

## Building & testing

Build and test through the repo driver — never run the `typed-geometry-test` binary directly:

```bash
uv run dev.py test            # build + run the full suite
uv run dev.py test "tg "      # just the typed-geometry tests while iterating
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — typed-geometry's documentation hub.
- [docs/modules/](docs/modules/scalar.md) — per-module "what belongs here / why is it this way" docs (e.g. the `pos + pos` translation rule, why `bivec != vec`).
- [docs/structure.md](docs/structure.md) — the full module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — tg-specific conventions (scalar traits, `data` storage, generic-over-`D` types), on top of the repo-wide ones.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows (`.clang-format` is authoritative for formatting).
