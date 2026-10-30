---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_decl.cc
  - file: libs/graphics/shaped-graphics-language/tests/check/array-check-test.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
corpus:
  - bgfx/shaders/06-bump/fs_bump
  - bgfx/shaders/09-hdr/fs_hdr_bright
  - bgfx/shaders/09-hdr/fs_hdr_lum
  - bgfx/shaders/09-hdr/fs_hdr_lumavg
  - bgfx/shaders/13-stencil/fs_stencil_color_lighting
  - bgfx/shaders/13-stencil/fs_stencil_texture_lighting
  - bgfx/shaders/36-sky/vs_sky
  - bgfx/shaders/37-gpudrivenrendering/fs_gdr_instanced_indirect_rendering
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-30T12:16Z
---

# Arrays and `mat3` in GPU memory, and matrices and arrays across a stage edge

An array is a value everywhere else (CHK-285).
It is `unsupported-yet` wherever GPU memory or a stage edge holds one (CHK-291).
`mat3` is a value of function bodies alone, refused wherever memory or a stage edge would hold one (CHK-397).
Their layout is decided (the spec's emitting file, "Open"), and they land together, after the tier-1 work.

- **In a constant block an array element starts a row**: `slib::row<T>` pads a shorter one to 16 bytes on the host, and WGSL reads it through `array<vec4f, N>`.
- **The mechanism that is missing** is a memory form holding an array, which a runtime index walks into.
- **A `mat3` is its three columns, each starting a row, in a block** (44 bytes), and `slib::gpu_mat3` holds that on the host.
  In a buffer's element it is 36 bytes, which is `tg::mat3f` itself.
- **WGSL passes no matrix and no array between stages**, so both cross as one location per column or element.

**It is the gap real shaders hit most.**
An array in a constant block blocks 8 of the first 104 bgfx translations in shader-rosetta, more than any other.
bgfx declares `uniform vec4 u_lightPosRadius[4]` and indexes it by a variable, or packs offsets as `uniform vec4 u_offset[16]`.
Each blocked shader compiled once its arrays were swapped for buffers, so nothing else hides behind it.
`corpus:` names them, under shader-rosetta's `sources/` (`uv run dev.py companions init shader-rosetta`).
