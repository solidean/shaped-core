---
created: 2026-10-17
refs:
  - file: extern/fidelityfx/CMakeLists.txt
  - file: libs/graphics/shaped-rendering/shaders/fsr3/ffx_core_hlsl.h
  - file: libs/graphics/shaped-shader-compiler-dxc/src/shaped-shader-compiler-dxc/impl/command_line_args.cc
  - symbol: sr::fsr_upscale_routine
---

# FSR's SPIR-V bindings come from a patch to AMD's `ffx_core_hlsl.h`

The patch is applied as extern/fidelityfx flattens AMD's sources.
AMD numbers each register class from zero, and SPIR-V would put `b0`, `t0`, `u0` and `s0` all at binding 0.
Under `__spirv__` the patch prefixes each class with its own digit instead — `t` becomes `1N`, `u` `2N`, `s` `3N`.
That holds while no constant buffer index reaches 10.

The real fix is per-class binding shifts (DXC's `-fvk-b-shift` and siblings) that a shader package can ask slib for.
The patch goes once slib has them.
The DXC wrapper passes no `-fvk-*-shift` deliberately today, so this is a slib-side feature first.
