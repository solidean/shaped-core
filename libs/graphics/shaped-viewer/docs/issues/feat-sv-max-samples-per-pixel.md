---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/render_settings.hh
  - symbol: sv::accumulation_sample_cap
---

# `render_settings::max_samples_per_pixel`, replacing the process-wide `accumulation_sample_cap`

It replaces `sv::accumulation_sample_cap` in `render_settings.hh`; per view rather than per process is the intended shape.
It is public rather than file-scope because a caller waiting for convergence has to tell "not there yet" from "as good as it gets".
`view_ref::is_accumulation_converged` applies it.
A layer stops at the smallest of `accumulation_sample_cap`, `viewer_definition::sample_target` where it is set, and `render_settings::max_samples_per_pixel` once it exists.
It must be excluded from the trace hash: lowering the cap should not restart the image.
