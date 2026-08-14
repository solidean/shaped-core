# shaped-core docs

The entry point for repo-wide documentation.
Library-local docs live next to their library, under `libs/<category>/<lib>/docs/`.

## Top-level

- [requirements.md](requirements.md) — the toolchain shaped-core assumes: compilers, CMake and Ninja, versions, and the rationale behind the minimums.
  `dev.py doctor` checks the core ones.
- [platforms.md](platforms.md) — the platform support model: which platforms are Tier 1 (CI-tested), Tier 2 (supported) or Tier 3 (planned), which build types should work, and the `SC_THREADS` knob.
- [libraries.md](libraries.md) — the catalog of shaped-core libraries: what each is, its namespace, and what it depends on.
- [graphics.md](graphics.md) — the graphics family (`sg`, `sr`, `sv`, plus the shader-side utilities) and how it fits together.
- [philosophy.md](philosophy.md) — the guiding stars behind the taste: who the libraries are for, how we pick tradeoffs, and why being opinionated is a feature.
- [coding-guidelines.md](coding-guidelines.md) — coding standards and design principles, including the prose style.
  `.clang-format` / `.clang-tidy` / `.clangd` are authoritative for formatting.
- [error-handling.md](error-handling.md) — `CC_ASSERT` versus `cc::result` / `optional` versus exceptions, the `try_*` + throwing-façade pattern, and how errors propagate.
  Repo-wide; affects all libs.
- [dev-py-driver.md](dev-py-driver.md) — the design behind the `dev.py` driver and its `tools/dev/` layers, and how to extend it or adapt it downstream.
  Rationale, not a how-to — usage lives in the building-and-testing guide.

## Notes

Findings we want to keep, but that are not a guide to anything.

- [notes/build-times.md](notes/build-times.md) — where a `dev.py check` spends its time, and the cold / semi-cold / warm scenarios it splits into.
  Also what was measured before choosing precompiled headers over unity builds, and before deferring a prebuilt extern.

## Guides

[guides/_index.md](guides/_index.md) lists all eleven; the ones most often wanted:

- [guides/building-and-testing.md](guides/building-and-testing.md) — drive `dev.py`, and diagnose with the `repo_tools` tools.
- [guides/ci.md](guides/ci.md) — the GitHub Actions workflows and what each uploads.
- [guides/prose.md](guides/prose.md) — `dev.py lint`, the prose rules in practice, and reworking a documentation surface.
- [guides/compile-times.md](guides/compile-times.md) — `dev.py compile-time`: which headers and TUs the compiler spends its time on.
- [guides/precompiled-headers.md](guides/precompiled-headers.md) — the per-target PCH tiers, how to pick one, and the no-PCH gate.
- [guides/disassembly.md](guides/disassembly.md) — `dev.py assembly`: read the emitted codegen, or trace what one invocation actually ran.
- [guides/profiling.md](guides/profiling.md) — hardware performance counters via `nx::bench` and `dev.py profiling`.
- [guides/coverage.md](guides/coverage.md) — LLVM source-based coverage, and [guides/pgo.md](guides/pgo.md) — profile-guided optimization.
- [guides/perf-results.md](guides/perf-results.md) — guide benchmarks and the `.perf.json` contract.
- [guides/cheat-sheets.md](guides/cheat-sheets.md) — the per-library cheat-sheet format, and where the sheets live.
- [guides/postmortem.md](guides/postmortem.md) — the session friction review behind `/postmortem`.

## Per-library docs

- [clean-core](../libs/base/clean-core/readme.md) — the `cc` foundational library.
  [cheat-sheet](../libs/base/clean-core/cheat-sheet.md) for the API at a glance, deeper notes in its [docs hub](../libs/base/clean-core/docs/_index.md).
- [nexus](../libs/base/nexus/readme.md) — the `nx` test framework.
  [cheat-sheet](../libs/base/nexus/cheat-sheet.md) for the API at a glance, deeper notes in its [docs hub](../libs/base/nexus/docs/_index.md).
- [typed-geometry](../libs/base/typed-geometry/readme.md) — the `tg` math & geometry library.
  [cheat-sheet](../libs/base/typed-geometry/cheat-sheet.md) and [docs hub](../libs/base/typed-geometry/docs/_index.md).
- [babel-serializer](../libs/io/babel-serializer/readme.md) — the `babel` format readers and writers.
  [cheat-sheet](../libs/io/babel-serializer/cheat-sheet.md) and [docs hub](../libs/io/babel-serializer/docs/_index.md).
- **graphics family** ([overview](graphics.md)) — shaped-graphics
  ([readme](../libs/graphics/shaped-graphics/readme.md) ·
  [cheat-sheet](../libs/graphics/shaped-graphics/cheat-sheet.md) ·
  [docs](../libs/graphics/shaped-graphics/docs/_index.md)), shaped-rendering
  ([readme](../libs/graphics/shaped-rendering/readme.md) ·
  [cheat-sheet](../libs/graphics/shaped-rendering/cheat-sheet.md) ·
  [docs](../libs/graphics/shaped-rendering/docs/_index.md) · [imgui](../libs/graphics/shaped-rendering/docs/imgui.md)), shaped-viewer
  ([readme](../libs/graphics/shaped-viewer/readme.md) ·
  [docs](../libs/graphics/shaped-viewer/docs/_index.md)), plus the shader side utilities
  shaped-shader-library ([readme](../libs/graphics/shaped-shader-library/readme.md) ·
  [cheat-sheet](../libs/graphics/shaped-shader-library/cheat-sheet.md) ·
  [docs](../libs/graphics/shaped-shader-library/docs/_index.md)) and shaped-shader-compiler-dxc
  ([readme](../libs/graphics/shaped-shader-compiler-dxc/readme.md) ·
  [cheat-sheet](../libs/graphics/shaped-shader-compiler-dxc/cheat-sheet.md)).
- **[shaders](../libs/graphics/shaped-graphics/docs/shaders.md)** — how the shader system works end to end: packages, `acquire(ctx)`, hot reload, shipping.
  It lives under shaped-graphics because that is where you look first, though most of the machinery is downstream of it.

## Tool docs

- [shaped-linter](../tools/shaped-linter/readme.md) — our own C++ and prose linter, and the `prose apply` plan grammar.
  Start at its [docs hub](../tools/shaped-linter/docs/_index.md), which covers the architecture, how to write a rule, and the corpus format.
- [instruction-tracer](../tools/instruction-tracer/readme.md) — what optimized code actually executed, driven by `dev.py assembly trace`.

> Place new repo-wide docs here, with kebab-case names, in the matching subfolder.
> [CLAUDE.md](../CLAUDE.md) carries the repo's working conventions.
