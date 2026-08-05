# typed-geometry

Strongly-typed C++23 math & geometry library, namespace `tg`.
Depends on **clean-core** for the vocabulary types (`i32`/`f32`/`isize`/…) and assertions.

```cpp
#include <typed-geometry/linalg/linalg.hh>

auto const a = tg::pos3f(1, 2, 3);
auto const b = tg::pos3f(4, 6, 3);
tg::vec3f const d = b - a;          // displacement between points
auto const dist = d.length();       // 5
```

Headers are included by their full path from `src/`, e.g. `#include <typed-geometry/linalg/vec.hh>`.
The root `fwd.hh` forward-declares the public types and defines the dimensional/typed aliases.

This library is at an **early stage**: `scalar/`, the whole `linalg/` core and the first `geometry/` primitives exist, and everything above them is still planned.
[docs/structure.md](docs/structure.md) is the roadmap, with a `[done]` / `[in progress]` / `[planned]` tag per module.

## Design at a glance

- **Semantic types.** `vec` is a displacement or direction, `pos` is a point, `comp` is the neutral component container.
  Their arithmetic reflects affine geometry: `pos - pos -> vec`, `pos + vec -> pos`, and the deliberate `pos + pos -> pos`.
- **One generic type per family.** `vec<int D, class T>`, and likewise `pos`/`comp`, with typedefs for D = 2/3/4 (`vec3f`, `vec2d`, `vec4i`) and no per-dimension specializations.
- **Raw storage, indexed access only.** Components live in a public C array member `data`, reached through `data` or `operator[]` — there are **no `.x/.y/.z`** members.
- **Extensible scalars.** Scalar capabilities route through `tg::scalar_traits<T>` rather than `std::`, so an expression tree, a double-double or a bigint can opt in.
  `length()`/`normalized()`/`distance()` exist only for scalars whose trait declares `has_sqrt`.

[docs/coding-guidelines.md](docs/coding-guidelines.md) carries each of these as a rule, and [docs/modules/](docs/modules/linalg.md) the reasoning behind it.

## File organization

Source lives in `src/typed-geometry/`, grouped by module:

| Folder      | What's in it |
|-------------|--------------|
| (root)      | `fwd.hh` (forward decls + aliases), `all.hh` (full umbrella) |
| `scalar/`   | the `scalar_traits<T>` seam, `tg::sqrt` and the trig functions, `angle`, `pi` |
| `linalg/`   | `vec`, `pos`, `comp`, `bivec`, `mat`, `quat` and their `_ops` free functions |
| `geometry/` | the `object_traits` seam and the primitives (`aabb`, `triangle`, `segment`, `ray`, `line`, `plane`) |

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
- [docs/modules/](docs/modules/scalar.md) — per-module "what belongs here / why is it this way"
  docs (e.g. the `pos + pos` translation rule, why `bivec != vec`).
- [docs/structure.md](docs/structure.md) — the full module roadmap.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — tg-specific conventions (scalar
  traits, `data` storage, generic-over-`D` types), on top of the repo-wide ones.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code
  follows (`.clang-format` is authoritative for formatting).
