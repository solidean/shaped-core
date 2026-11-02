---
created: 2026-10-17
parent: epic-sv-denoising-integration
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/view/render_settings.hh
---

# `render_settings::exposure`, for the tonemap

For the tonemap when it lands.
`dlss_rr` and the fsr upscaler already read `reconstruct.exposure`, and NRD deliberately does not.
