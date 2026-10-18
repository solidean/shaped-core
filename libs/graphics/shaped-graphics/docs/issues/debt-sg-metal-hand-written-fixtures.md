---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/tests
  - test: ssc::msl end to end - the reflected bindings build the layout the backend encodes
---

# The metal tier-2 fixtures are hand-written and hand-compiled

The tier-2 fixtures are `xxd -i` dumps — `double_compute.metallib.h` and its neighbours — with their reflection written by hand beside them.
`raytrace.metal` and `mesh.metal` are hand-written MSL too.

**The agreed shape is HLSL run through SPIRV-Cross, with the HLSL, the generated MSL and the metallib all checked in.**
The argument-buffer layout this backend encodes was chosen to match what SPIRV-Cross emits.
So a fixture written by hand can agree with the backend and still disagree with every real shader.
Neither DXC nor SPIRV-Cross runs on an arm64 macOS host today, which is why the hand-written ones exist in the meantime.
That route is [feat-slib-hlsl-to-metal-route](../../../shaped-shader-library/docs/issues/feat-slib-hlsl-to-metal-route.md).

**The nearer step is compiling them through `shaped-shader-compiler-msl` at build time**, which would drop the dumps.
The cost is that the tier-2 binary would then require the Metal toolchain component, a trade worth making deliberately rather than in passing.
`ssc::msl end to end - the reflected bindings build the layout the backend encodes` now checks that a reflected layout and the encoded one agree.
That is the property the hand-written fixtures could never pin.
