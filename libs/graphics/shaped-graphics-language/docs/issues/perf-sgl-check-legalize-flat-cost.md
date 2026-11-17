---
created: 2026-11-17
priority: next
tags: [sgl, perf, ci]
refs:
  - test: "sgl checked modules - behind a layer of the whole library*"
  - file: libs/graphics/shaped-graphics-language/tests/check/checked-prelude-test.cc:861
---

# Checking and legalizing a shaped-viewer program costs ~0.5 s optimized and ~20x that in Debug, spread across every pass

**The test above outgrows nexus's 30 s no-progress watchdog on slow builds**: Linux Clang Debug exits 4 in it, and the WASM threads job's 60 s budget for the binary runs out.
It narrows itself on those builds until the checker is faster (`TEMPORARY(perf-sgl-check-legalize-flat-cost)` in the test).

## Measured, 2026-11-17, Windows, one test alone

- relwithdebinfo-clang: 2.2 s for the default four programs plus one whole-library check, 495 MiB peak.
- debug-clang: 41.6 s with the watchdog off (`-- --watchdog 0`), 542 MiB peak.
- Sampled (`dev.py test "<name>" --sample`), of the test thread's time:
  the test's `observed_as` 36% (legalize and emit for the comparison), `sgl::check::check` 22%,
  and within them `legalize` 30%, flattening 19%, emit 15%, `simplify_lets` 11%, `judge_uniformity` 11%.
- **No hotspot**: the top self-time functions are 1–3% each.
  They span `expr_lowering`, `let_simplifier` (`find_written`, `count_reads_of`, `writes_local`), `checker::match`, `constant_folder`, `void_eraser`, the text writer, and allocation.
- `cc::map<cc::string, cc::unit>::copy_from` shows at ~2%: string sets copied per call somewhere in the checker.

## Done when

The test runs its default four programs inside the watchdog on Linux Clang Debug and in the WASM threads job, and the narrowing marked with this issue is gone.
