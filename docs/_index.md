# shaped-core docs

The entry point for repo-wide documentation. (Library-local docs live next to their library under
`libs/<category>/<lib>/docs/`.)

## Top-level

- [requirements.md](requirements.md) — the toolchain shaped-core assumes (compilers, CMake/Ninja,
  versions) and the rationale behind the minimums; `dev.py doctor` checks the core ones.
- [platforms.md](platforms.md) — the platform support model: which platforms are Tier 1 (CI-tested),
  Tier 2 (supported), or Tier 3 (planned), and which build types should work.
- [libraries.md](libraries.md) — catalog of the shaped-core libraries: what each is, its
  namespace, and the dependency order.
- [coding-guidelines.md](coding-guidelines.md) — coding standards and design principles for the
  shaped-core libraries. `.clang-format` / `.clang-tidy` / `.clangd` are authoritative for
  formatting.
- [dev-py-driver.md](dev-py-driver.md) — the design & philosophy behind the `dev.py` build/test
  driver and its `tools/dev/` layers, and how to extend it or adapt it downstream (rationale, not
  a how-to — usage lives in the building-and-testing guide).

## Guides

See [guides/_index.md](guides/_index.md) for the full list. Most useful:

- [guides/building-and-testing.md](guides/building-and-testing.md) — how to drive `dev.py` and the
  `repo_tools` diagnostics.
- [guides/ci.md](guides/ci.md) — the GitHub Actions CI workflows and diagnosing failures with `gh`.
- [guides/cheat-sheets.md](guides/cheat-sheets.md) — the per-library API cheat-sheet format and
  where the sheets live.
- [guides/pgo.md](guides/pgo.md) — profile-guided optimization (`dev.py pgo`) and
  [guides/perf-results.md](guides/perf-results.md) — guide benchmarks + the `.perf.json` contract.
- [guides/postmortem.md](guides/postmortem.md) — the session friction review (`/postmortem`).

## Per-library docs

- [clean-core](../libs/base/clean-core/readme.md) — the `cc` foundational library;
  [cheat-sheet](../libs/base/clean-core/cheat-sheet.md) for the API at a glance, deeper notes in
  its [docs hub](../libs/base/clean-core/docs/_index.md).
- [nexus](../libs/base/nexus/cheat-sheet.md) — the `nx` test framework; the
  [cheat-sheet](../libs/base/nexus/cheat-sheet.md) covers `TEST`/`CHECK`/`SECTION`, and
  [catch2-runner-compat](../libs/base/nexus/docs/catch2-runner-compat.md) covers how nexus test
  binaries are discovered and filtered.

> Place new repo-wide docs here (kebab-case names), in the matching subfolder. See
> [CLAUDE.md](../CLAUDE.md) for the repo's working conventions.
