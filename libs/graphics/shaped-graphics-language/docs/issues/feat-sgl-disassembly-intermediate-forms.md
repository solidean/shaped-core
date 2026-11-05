---
created: 2026-11-05
parent: epic-sgl-shader-disassembly
---

# Show each target's intermediate form of an entry point: DXIL, SPIR-V, WGSL and AIR, at the .sgl line

`sgl emit` writes SGL's own text; this goes one step further, through the compiler slib would hand it to, with the flags slib would hand it.
DXIL and SPIR-V through DXC's disassembly, WGSL as Tint or Naga hands it on, and a metallib's AIR on macOS.
Each instruction is attributed to the `.sgl` line its text came from, through EMIT-181's line table and the target compiler's own debug lines.
The flags matter as much as the code: `fast` on DXIL, `NoContraction` and the float execution modes on SPIR-V, which today are read by hand.
