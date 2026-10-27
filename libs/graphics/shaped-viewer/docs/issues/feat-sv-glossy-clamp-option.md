---
created: 2026-10-17
priority: someday
refs:
  - symbol: sv::render_settings
---

# An opt-in clamp for glossy chains' fireflies

Glossy chains sparkle, and stay unbiased by decision.
The quadric gallery's fireflies near the shapes' bases are light the glossy floor reflects off the glossy shapes: rare paths, each carrying up to about 2 × 2 × the light's radiance.
Both tracers converge to the right image, and making every surface fully rough removes them, so no sampler or intersection is at fault.

The remedies all trade bias for variance.
One clamps what a sample brings back from beyond its first hit (Cycles' "clamp indirect"); the other raises a later vertex's roughness to the path's so far (Cycles' "filter glossy").
Neither is taken; one would be a `render_settings` member, off by default.
