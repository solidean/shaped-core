---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/shader-logging.md
  - file: libs/graphics/shaped-graphics-language/readme.md
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-02T15:48Z
---

# `print` and `assert` on the GPU

`print` and `assert` run on the interpreter only; `legalize` removes them from every target's text.
[incubator/shader-logging.md](../spec/incubator/shader-logging.md) sketches how they would reach a GPU, and nothing of it is built.

A portable form needs no target support: a buffer the host reads back, written through an atomic cursor, on all four targets.
The readme lists "assertions, logging, per expression traces" among what SGL offers, which holds for the interpreter alone until this lands.
