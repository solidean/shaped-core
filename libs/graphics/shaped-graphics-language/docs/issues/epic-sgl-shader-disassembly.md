---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/emit_command.cc
  - file: docs/guides/disassembly.md
---

# Shader disassembly per target, and later machine simulation, as part of SGL's tooling

On the CPU, `dev.py assembly` is a local godbolt: `search` finds a symbol, `show` disassembles it, `trace` says what one call ran ([disassembly.md](../../../../../docs/guides/disassembly.md)).
SGL wants the same for shaders: from an `.sgl` entry point to what each target's compiler made of it, at the `.sgl` line.
Today `sgl emit` stops at the text SGL writes, and everything after it, DXC, Tint, Metal and the driver, is a black box.

Three layers, each a child:

- **The intermediate form each target compiles to**: DXIL from DXC, SPIR-V from DXC's SPIR-V backend, the WGSL Tint or Naga hands on, and the AIR of a metallib.
  These are what the float guarantees and the guards are judged on: `fast` flags, `NoContraction`, the execution modes, whether a guard survived.
- **The ISA the GPU runs**, from the vendors' own tools where they exist: AMD's Radeon GPU Analyzer, Intel's shader dumps, NVIDIA's, Apple's GPU tools.
  This is where the Intel driver's `(1 / x) * x` cancellation would have been visible at a glance.
- **Machine simulation, later**: register pressure, occupancy and cycle estimates per entry point, so an optimization is judged before it is run on hardware.

Each answer names the `.sgl` line it came from, through the line table SGL already writes (EMIT-181), so an agent can read a shader's cost the way it reads a function's.
