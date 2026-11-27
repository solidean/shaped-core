---
created: 2026-11-22
refs:
  - file: libs/graphics/shaped-graphics/tests/routine/routine-test.cc:537-556
  - test: sg - an idle registry's idle_completion is ready when asked
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-27T14:56Z
---

# The idle-registry test fails now and then under a full run

`ctx->routines.idle_completion()->is_ready()` failed once in a full `dev.py ship` run on 2026-11-22, on the hardware dx12 context, and passed three full `shaped-graphics-test` runs straight after.

Hypothesis, not confirmed: the test takes the context's registry as idle when it starts, and is exclusive against `sg-reload-generation` alone.
Another test registering a routine on the same shared context in parallel leaves the registry pending, which fails the first check.
If so, the test wants an exclusive group every routine-registering test shares, or a context of its own.
