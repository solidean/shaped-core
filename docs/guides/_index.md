# Guides

Task-oriented how-tos for working in shaped-core.
(Back to [docs/_index.md](../_index.md).)

- [building-and-testing.md](building-and-testing.md) — drive `dev.py` (build / test / doctor /
  presets) and diagnose failures with the `repo_tools` `build_diag` / `test_diag` tools.
- [examples.md](examples.md) — the `examples/` corpus and `dev.py example`: what an example is for next to a test, how `EXAMPLE` and the CMake helpers work, and what binds one.
- [prose.md](prose.md) — `dev.py lint`'s prose half: when to run it, why dirty-only is line-exact,
  and when a pile of findings is a rework rather than a run of local edits.
- [disassembly.md](disassembly.md) — `dev.py assembly`: `search` / `show` are a local godbolt over the built object files, for reading the optimizer's actual codegen.
  `trace` is the dynamic half — what one invocation really ran, the data it touched, and a static cost model over that.
- [compile-times.md](compile-times.md) — `dev.py compile-time`: what a header costs to include and how much of a TU is its includes, plus `build --files` to compile a glob and nothing else.
  The tool half of [notes/build-times.md](../notes/build-times.md).
- [precompiled-headers.md](precompiled-headers.md) — the per-target PCH tiers, why a bigger one is not automatically better, and the SDK-gate ordering rule.
  Also the no-PCH presets that keep `/FI` from masking a missing include.
- [profiling.md](profiling.md) — measure what the code actually cost at run time: nexus/bench hardware
  performance counters (instructions, cache misses, branch mispredicts) via `nx::bench` and
  `dev.py profiling`.
- [ci.md](ci.md) — the per-platform GitHub Actions workflows (Windows / Linux clang), what they
  run via `dev.py`, and diagnosing failures with the `gh` CLI.
- [coverage.md](coverage.md) — collect LLVM source-based test coverage with `dev.py coverage`
  (run / merge / report) and the `.llvm-cov.json` sidecar tooling can build on.
- [pgo.md](pgo.md) — profile-guided optimization with `dev.py pgo`
  (instrument / train / optimize / measure), trained and measured via PGO benchmarks.
- [perf-results.md](perf-results.md) — PGO benchmarks (`PGO_BENCHMARK` + `nx::pgo`) and the
  `.pgo.json` metric sidecar contract that `dev.py pgo` builds on.
- [dependencies.md](dependencies.md) — the `extern/<dep>/dependency.yml` manifests, `dev.py deps list` / `deps licenses`, and the bump workflow.
  Also the license allowlist that `dev.py check` gates on.
- [cheat-sheets.md](cheat-sheets.md) — what a library cheat sheet is, its format, and where the
  colocated per-library sheets live (start with clean-core and nexus).
- [postmortem.md](postmortem.md) — the end-of-session friction review behind the `/postmortem`
  skill: where momentum was lost, not a bug report.
- [reviewing-prs.md](reviewing-prs.md) — what we look for when reviewing a PR, ranked by what it costs to get wrong, and the tradeoffs that have no absolute answer.
  The living half of the `reviewing-a-pr` skill, written from actual reviews.

> Place new guides here with kebab-case names.
