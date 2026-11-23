---
created: 2026-11-22
tags: [metal, porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_context.cc
  - test: sg - a quad vote is its whole quad's, so a derivative under a branch on one is its quad's (CHK-533)
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-23T10:48Z
---

# Metal grants quad_control and multi_viewport once a run on Apple hardware shows each works

SGL writes `quad_any` and `quad_all` for MSL as Metal's own `quad_any` and `quad_all`.
sg's metal backend reports `sg::feature::quad_control` as absent, so no metal context runs them yet.

Whether Metal's helper threads take part in quad-group functions in a fragment function was not read from Apple's specification: the PDF was too large to fetch.
A run of the GPU test above on Apple hardware answers it, since its derivative is exact only where the asking quad's helpers voted.
If it passes, the metal backend grants the feature, with the specification's wording quoted in its comment if it can be found.

`multi_viewport` waits the same way: SGL writes `[[viewport_array_index]]`, and the backend has no `raster_set_viewports` of its own yet.
It would call `setViewports:count:` and `setScissorRects:count:`, and the test `sg - a vertex stage sends each instance to its own viewport, each with its own scissor (CHK-534)` shows whether it does.
