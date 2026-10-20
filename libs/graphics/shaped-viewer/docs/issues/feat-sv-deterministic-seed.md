---
created: 2026-10-19
refs:
  - symbol: sv::seed_of
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - file: libs/graphics/shaped-viewer/docs/wavefront.md
---

# A seed that makes a capture the same image on any machine

A traced layer's seed hashes its slot's restart count with its frame, and how many restarts come before a capture's final run depends on timing: cold compiles and owed-work drains each add one.
So two runs of one capture hold the same sample count but not always the same image.

The seed needs a hash of machine-independent inputs instead.
`trace_hash` cannot serve as one: it folds in `inst.blas.get()` and the records' bindless indices, which differ between machines and between runs.
Each pointer and slot index has to be replaced by its content identity before it is hashed.

Done when a test captures one scene in two processes, with the restarts between them made to differ, and the two images are equal bit for bit.
