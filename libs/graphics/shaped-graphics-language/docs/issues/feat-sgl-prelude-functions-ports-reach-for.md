---
created: 2026-10-30
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-30T19:15Z
---

# A half's bits, the prelude function ports still reach for

`radians`, `degrees`, a floored `mod`, `b as float`, `mat4 * hpos4` and scalar-edged `smoothstep` have landed (CHK-405, CHK-197, CHK-406, EMIT-166).
What is left is the one that needs a design:

- **A `half`'s bits**, `h.bits` and `half.from_bits`, as `float` has them: FSR's approximate reciprocals work on them, marked `// gap:` in cs_fsr_easu_16 and cs_fsr_rcas_16 in shader-rosetta.
  The bits are a `ushort`, which WGSL has no spelling for (EMIT-109), so either they travel as the low 16 bits of a `uint` on every target, or the operation needs `shader_int16` and WGSL refuses it.
