---
created: 2026-11-05
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-float-specials-test.cc
  - test: "sg - an SGL float keeps NaNs, infinities, signed zeros and the order of a sum on the GPU"
closed: 2026-11-05
resolution: fixed
---

# Intel's vulkan driver also loses a strict 0 * inf, on the same Arc B580

The float-specials probe, which vulkan runs since `7c54666c9`, fails on the vulkan arm of `shaped-graphics-test` on an Intel Arc B580.
`cc::is_nan(got[2])` fails at line 61, and the comparison at line 72 reads 0 where 1 is expected.
Index 2 is the probe's `inf * zero`, the one check `bug-sg-intel-dx12-zero-times-inf` sees fold to 0 on that adapter's dx12 driver.
So the likeliest reading is the same driver simplification on the vulkan side; the SPIR-V has not been read yet to confirm it keeps the multiply strict.
Vulkan SDK 1.4.363.1; the driver version is the one the dx12 issue names, unconfirmed for vulkan.

## Resolved the same day

The SPIR-V, read: the comparison at line 72 is `nan != nan`, which DXC lowers to SPIR-V as `OpFOrdNotEqual`, false for a NaN.
That was SGL's own miscompile on every vulkan device, and HLSL now spells a float `!=` through `==` (EMIT-186).
Index 2 is `(one / zero) * zero`, which the Arc's vulkan driver cancels to 1 as its dx12 driver does.
[bug-sg-intel-dx12-zero-times-inf](../../../bug-sg-intel-dx12-zero-times-inf.md) now covers both, and the test skips that one check on Intel's hardware adapters.
