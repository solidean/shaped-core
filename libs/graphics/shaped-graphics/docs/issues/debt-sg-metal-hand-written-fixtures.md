---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/tests
  - file: libs/graphics/shaped-graphics/backends/metal/tests/metal-sgl-test.cc
  - test: ssc::msl end to end - the reflected bindings build the layout the backend encodes
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-05T12:43Z
---

# The metal tier-2 fixtures are hand-written and hand-compiled

The tier-2 fixtures are `xxd -i` dumps — `double_compute.metallib.h` and its neighbours — with their reflection written by hand beside them.
`raytrace.metal` and `mesh.metal` are hand-written MSL too.
So a fixture can agree with the backend's argument-buffer convention and still disagree with every shader a real pipeline produces.

**The target shape is SGL packages, as `metal-sgl-test.cc` already is.**
SGL is the one source promised on every target, so it is what real metal shaders are written in.
HLSL is not a route: it does not reach metal, by decision ([feat-slib-hlsl-to-metal-route](../../../shaped-shader-library/docs/issues/.closed/2026/11/feat-slib-hlsl-to-metal-route.md)).
A fixture SGL cannot say yet, such as a ray-tracing entry point it does not support, stays hand-written, and that gap belongs to an SGL issue.

**The cost is a dependency the tier-2 binary avoids today**: an SGL package needs slib and the metal compiler edge (`SC_HAS_METAL_COMPILER`).
`metal-sgl-test.cc` already takes it behind that gate; moving the rest is the same trade made for the whole binary, deliberately rather than in passing.
`ssc::msl end to end - the reflected bindings build the layout the backend encodes` checks that a reflected layout and the encoded one agree.
That is the property the hand-written fixtures could never pin.

**Done on 2026-11-05 for everything but ray tracing.**
`kernels.sgl` and `draws.sgl` replace `double_compute.metal`, `triangle.metal` and `mesh.metal`.
The compute, raster, barrier and vertex-input tests take their shaders and bindings from the package.
The tier-2 binary links slib unconditionally now, which costs no toolchain: the metal compiler emits MSL source where there is none.
`raytrace.metal` and `raytrace_second.metal` remain, because their tests hand the backend's pipeline description metal functions directly, which SGL's generated pipelines do not spell.
