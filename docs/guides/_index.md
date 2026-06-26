# Guides

Task-oriented how-tos for working in shaped-core. (Back to [docs/_index.md](../_index.md).)

- [building-and-testing.md](building-and-testing.md) — drive `dev.py` (build / test / doctor /
  presets) and diagnose failures with the `repo_tools` `build_diag` / `test_diag` tools.
- [coverage.md](coverage.md) — collect LLVM source-based test coverage with `dev.py coverage`
  (run / merge / report) and the `.llvm-cov.json` sidecar tooling can build on.
- [pgo.md](pgo.md) — profile-guided optimization with `dev.py pgo`
  (instrument / train / optimize / measure), trained and measured via guide benchmarks.
- [perf-results.md](perf-results.md) — guide benchmarks (`GUIDE_BENCHMARK` + `nx::guide`) and the
  `.perf.json` metric sidecar contract that `dev.py pgo` builds on.
- [cheat-sheets.md](cheat-sheets.md) — what a library cheat sheet is, its format, and where the
  colocated per-library sheets live (start with clean-core and nexus).
- [postmortem.md](postmortem.md) — the end-of-session friction review behind the `/postmortem`
  skill: where momentum was lost, not a bug report.

> Place new guides here with kebab-case names.
