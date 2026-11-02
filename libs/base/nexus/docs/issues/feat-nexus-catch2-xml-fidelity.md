---
created: 2026-10-17
refs:
  - file: libs/base/nexus/src/nexus/tests/export/catch2.cc
  - symbol: nx::write_catch2_results_xml
---

# Catch2 XML output: what the reporter leaves out

- Emit captured StdOut / StdErr elements, useful for failure diagnostics and hung tests.
- Emit chained `.context()` / `.note()` / `.dump()` as their own `<Info>` elements.
  Today they ride along inside `<Expanded>`, which reads fine but does not let an IDE separate them from the expression.
- Model partial test-case runs (SECTION re-entry / partNumber) instead of only a merged section tree.
- Add benchmark result reporting hooks, even if unimplemented for now.
- Include run metadata (run name, RNG seed) for reproducibility and debugging.
- Track and emit expectedFailures properly instead of hardcoding 0.
- Fill discovery `<Tags>` from declarations; tag filtering is a core Catch2 feature.
- Consider emitting explicit "test/section started" progress lines on stderr, for live IDE feedback.
