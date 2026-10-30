---
created: 2026-10-30
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-30T19:15Z
---

# Prelude functions every port reaches for: radians, a floored mod, bool to float, mat4 * hpos4

The 368 bgfx translations in shader-rosetta keep writing the same small helpers, each a function every source language has.

- **`radians(d)` and `degrees(r)`**: 30 shadowmap variants define their own, marked `// gap:`.
- **A floored `mod(a, b)`**, GLSL's: SGL's float `%` truncates, as HLSL's `fmod` does, and three translations define `a - b * floor(a / b)`.
- **`b as float` of a `bool`**: `no-matching-overload: bool as float: no conversion` today, so 14-shadowvolumes and 16-shadowmaps write `select(b, 1.0, 0.0)` about twenty times.
- **`mat4 * hpos4`**: `mat4 * pos3` gives an `hpos4`, and the next matrix of a model-then-view-projection chain takes none, so the translations go through `float4` (debugdraw).
- **`smoothstep` of scalar edges and a vector**, as `min`, `max` and `clamp` now take scalar bounds (CHK-406): fs_font_distance_field_subpixel.
- **A `half`'s bits**, `h.bits` and `half.from_bits`, as `float` has them: FSR's approximate reciprocals work on them, marked `// gap:` in cs_fsr_easu_16 and cs_fsr_rcas_16.
