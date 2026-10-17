---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
---

# A ray escaping while still inside a solid is dropped, silently

It travelled an unbounded distance through an absorbing medium, so nothing survives.
That is only true for CLOSED geometry.
An open shell authored as solid loses paths rather than being told it is wrong.
Whether and where to report it is the question.
