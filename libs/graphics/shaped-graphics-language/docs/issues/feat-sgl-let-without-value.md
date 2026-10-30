---
created: 2026-10-30
corpus:
  - bgfx/shaders/53-sky2/cs_atmo_skyview
  - bgfx/shaders/53-sky2/fs_sky2
  - bgfx/shaders/53-sky2/fs_sky2_bg
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-30T16:08Z
---

# A let without a value, assigned on every path before it is read

`let mut v: float` followed by an `if` whose branches each assign `v` is `unsupported-yet: a let without a value` (CHK-55).
GLSL and HLSL write it constantly: a value whose branches need their own locals before they reach it, as bgfx's atmosphere.sh does in `atmosphere_skyview_uv`.

- **Wanted**: a `let mut` with a written type and no value, read only after every path to the read has assigned it.
  That is a definite-assignment analysis over the structured body, the one place it is new; the flat tree already has `var` with no initializer.
- **The workaround** is an `if` value where each branch is one expression, which fails as soon as a branch needs a local of its own.
- It blocks 3 of the 209 bgfx translations in shader-rosetta.
