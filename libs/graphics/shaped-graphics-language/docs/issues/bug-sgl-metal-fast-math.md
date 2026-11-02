---
created: 2026-11-02
priority: next
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_common.cc
  - file: libs/graphics/shaped-shader-compiler-msl/src/shaped-shader-compiler-msl/compiler.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/evaluation.md
---

# metal compiles SGL's MSL with fast math on

EVAL-6 says a `float` is a 32-bit IEEE number, and metal's compiler defaults to fast math.
Neither path turns it off: sg's metal backend compiles MSL source with default `MTL::CompileOptions`, and `ssc::msl` passes no `-fno-fast-math` to `xcrun metal`.
So a NaN, an infinity, a signed zero or a reassociated sum can differ on metal from the other targets; only `is_nan` and `is_inf` are made robust, by testing bits.

The fix is likely both compile paths asking for precise math (`mathMode` / `-fno-fast-math`), measured for its cost on sv's path tracer.
DXC's reassociation (no `-Gis`) and denormal handling are the same question on dx12 and vulkan, and the spec should then say what holds for floats on every target.
