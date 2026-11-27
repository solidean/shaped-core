---
created: 2026-11-27
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - symbol: CHK-282
  - symbol: CHK-283
---

# Implicit derivatives under a branch the checker can prove the same across each quad

CHK-282 refuses an implicit sample, `ddx`/`ddy` and `fwidth` in non-uniform control flow, and CHK-283 calls every stage input non-uniform, a flat one included.
Some branches are provably taken alike by every invocation of a quad, helpers included, and an implicit sample under them is well defined on the APIs that specify it.
Accepting those would let such code be written as HLSL writes it, without a workaround that costs exactness or work.

## Where it came from

Quest3D (2026-11-27) samples a bindless material texture with a mip bias, under a branch on whether the per-draw texture index is valid:

```sgl
let d = pass.gDraws[input.drawRecord]          // drawRecord: @interpolate(.flat)
let idx = if d.texOverride != kInvalid => d.texOverride else pass.gMaterials[d.materialIdx].tex
if idx != kInvalid:                             // refused today: idx comes from a stage input
    albedo = textures[idx].sample(input.uv, aniso, bias = -2.0 + globals.mipBias)
```

The workarounds it weighed:

- **Gradients taken before the branch, scaled by `exp2(bias)`**, sampled with gradients inside it.
  The mip level matches a bias, but under anisotropic filtering the scaled gradients also shorten the footprint along the long axis.
  Measured on an RTX 5080: +34% sharpness on a stone at a grazing angle, half the covered pixels more than one 8-bit level off.
- **A sampler that carries the bias**, with unscaled gradients: exact on D3D12 and Vulkan, but Metal's and WebGPU's samplers have no bias.
  No target has a sample that takes both a bias and gradients: SPIR-V's `Bias` is implicit-lod only, DXIL's `SampleGrad`, MSL's `gradient2d` and WGSL's `textureSampleGrad` take none.
- **The sample taken before the branch at an always-valid fallback index**, the default picked after it.
  Exact and portable, and cheap here since the index is per draw.
  This is what Quest3D does for now.

The last one is fine where a valid fallback exists and the discarded read is cheap.
Where neither holds, a checker that can prove the branch uniform across the quad is the only exact route.

## What the APIs guarantee

- **Vulkan**, Derivative Operations: a derivative group is the primitive scope instance unless `QuadDerivativesKHR` is declared, then the quad scope instance.
  Scope: the invocations of one primitive form a primitive scope instance, helpers included, and each quad scope instance lies within one.
- **D3D11.3** §16.8.1 calls a value varying if it "could have different values across different Pixel Shader invocations on a single primitive".
  §16.8.2 forbids implicit samples only inside varying flow control.
  A fetch from a read-only resource at a non-varying address is non-varying; a result depending on UAV memory may not feed a derivative (§16.13).
- **SPIR-V** §2.19: derivatives are well defined where the derivative group has uniform control flow.
- **WGSL** scopes derivative uniformity to the whole draw and treats every user input as non-uniform (§15.2).
  Its text would need `@diagnostic(off, derivative_uniformity)`, and the result then rests on the backend underneath.
- **Metal** only says derivatives are undefined in non-uniform control flow (MSL §6.11.1.1) and defines no scope.

## A possible shape

A uniformity level between uniform and non-uniform, **primitive-uniform**: the same for every invocation of one primitive, helpers included.

- Flat inputs and the primitive's id are primitive-uniform; so are uniform values.
- Pure operations on primitive-uniform values, and loads from read-only memory at primitive-uniform addresses, stay primitive-uniform.
- Interpolated inputs, the position, coverage, the sample index, the helper flag, subgroup operations, atomics and `mut` memory are not.
- CHK-282 then accepts a derivative under control flow that is primitive-uniform.

Primitive-uniform implies quad-uniform under every API's default.
A value only the same per quad (built from `floor(position / 2)`, say) would need `QuadDerivativesKHR` on Vulkan and has no D3D equivalent, so it is a later question.

## Conditions to settle

- `discard` must demote, not terminate, on SPIR-V (`OpDemoteToHelperInvocation`), as it already does on DXIL, WGSL and Metal 2.3.
- Whether Metal and WGSL are served by practice alone, or SGL refuses the relaxation on them (a named feature, as backend gaps are).
- An adversarial GPU test on D3D12 and Vulkan: two triangles sharing an edge with different flat values, quads straddling it holding helpers of both.
  Each triangle's biased sample under a branch on its flat value must match an unbranched sample exactly.
- How far the proof reaches at first: flat inputs and uniforms only, or read-only loads indexed by them too.
