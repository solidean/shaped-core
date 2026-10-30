---
created: 2026-10-30
corpus:
  - bgfx/shaders/54-s2h/fs_s2h_gather
  - bgfx/shaders/54-s2h/fs_s2h
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-30T19:00Z
---

# A constant array is rebuilt at every use, which multiplies a shader's text

A file-scope `const` table, S2H's 192-entry `g_mini_font`, is written out in full wherever it is indexed once calls are inlined.
HLSL and MSL assign all 192 elements before each `g_mini_font[i]`, and WGSL repeats the whole `array<u32, 192>(…)` literal.

- **Measured**: fs_s2h_gather, about 300 characters printed, emits about 1.5 MB of HLSL and MSL and 0.55 MB of WGSL; fs_s2h, 11 characters, about 90 KB.
  bgfx's own output for the same shader is 64 KB of HLSL and 455 KB of MSL.
- **Wanted**: a constant composite that is indexed at run time is declared once per target text: HLSL `static const`, MSL `constant`, WGSL a module-scope `const`, and each use reads it.
- **Where**: the flattener writes a composite const as a construction at each use (`constant_value` → `value_of` in flatten.cc), and nothing shares it afterwards.
