---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/impl/debug_windows.cc
  - test: sg - submission tokens advance across submits
---

# A debug window's readback is async work a test that never drains its epochs leaves behind

With every SGL shader guarded under `report` (SC_SHADER_UNDEFINED=REPORT), 4 sg tests and 1 sr test failed on 2026-11-05 with only `left async work running — 1 async item(s) still carry its context`.
They differ run to run, and none of them logs a guard report.
The hypothesis, not yet traced, is sg's readback of a debug window, queued when an epoch retires, carrying the context of whichever test advanced it.
Without guards no package shader writes records, which is why no default run shows it.
It blocks `report` as a dev default: either the readback holds no test's context, or retiring an epoch drains it.

- **2026-11-12: the sg, sr and sv drivers settle each test's records inside it.**
  `nx::invocation_options::finish_each` awaits `ctx.diagnostics_settled()` as each child's last step, so the readback a test's shader caused completes before its leak check counts.
  Not rechecked under `report` yet: that rerun is what closes this.
