---
created: 2026-10-17
parent: epic-sv-denoising-integration
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/render_settings.hh
  - symbol: sv::view_renderer::_schedule_denoise
  - symbol: sv::render_settings
closed: 2026-10-17
resolution: fixed
---

# `render_settings::render_scale`, which plugs sv into FSR and `dlss_rr`

With it the plan traces at `sr::reconstruct_input_extent` instead of the view's own size.
Until then `_schedule_denoise` sets `reconstruct.upscaler` to `none`, which is what this lifts.
`render_settings::reconstruct`'s `upscaler`, `scale`, `upscale_sharpness` and `frame_time_ms` are not read until then.
