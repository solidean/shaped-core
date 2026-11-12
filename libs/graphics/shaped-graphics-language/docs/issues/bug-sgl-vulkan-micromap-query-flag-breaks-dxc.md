---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
  - test: sv vulkan - every mode and every denoiser draws the view it draws on dx12
---

# Every vulkan ray query flagged for micromaps makes DXC take minutes and emit invalid SPIR-V

Since 5a008f936 (EMIT-184 on vulkan), a vulkan compile for a device with `opacity_micromap` writes every ray query as `RayQuery<…, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>`.
The entry point also declares `SPV_EXT_opacity_micromap` and capability 5381.
On sv's large kernels DXC then takes minutes, and on the deferred hybrid kernel it fails SPIR-V validation:

```
fatal error: generated SPIR-V is invalid: [VUID-StandaloneSpirv-OpTypeImage-06924] Cannot store to OpTypeImage, OpTypeSampler, OpTypeSampledImage, or OpTypeAccelerationStructureKHR objects
  OpStore %12367 %27176
```

sv on vulkan breaks with it on any device that has micromaps: the deferred hybrid frame is declined.
The forward hybrid's kernels compile, in about 250 s, longer than nexus' watchdog allows a test to wait.
So "sv vulkan - every mode and every denoiser draws the view it draws on dx12" fails, and hangs the default run.

Measured, 2026-11-05, with the pinned DXC 1.9.2602 (`extern/dxc`), `-O3 -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout`, `cs_6_6`.
The input was `hybrid_pass.sgl`'s `light_thin`, emitted with `--diagnostics --line-directives` and the device features of an NVIDIA vulkan context:

| device features | DXC time | result |
|---|---|---|
| all but `opacity_micromap` | 18 s | valid |
| with `opacity_micromap` | 245 – 359 s | invalid SPIR-V, the error above |

The only difference between the two emitted texts is the template flag on each query and the capability on the entry point.
Which of the two DXC chokes on is not yet separated.
Reproducing needs a DXC command line, which `extern/dxc` does not install, and the Windows SDK's `dxc.exe` has no SPIR-V codegen.
A small ctypes driver over the pinned `dxcompiler.dll` does it.

Before the commit vulkan saw micromaps without the flag.
The commit added it because DXC takes `RAY_FLAG_FORCE_OMM_2_STATE` only beside it.
A likely fix is to write the flag and the capability on vulkan only for a query whose ray is forced to 2 states.
That leaves every other query as it was.

## Log

2026-11-12: it hung the default `shaped-viewer-test` run twice, once a change to modules `light_thin` uses (`quadric_runtime`, `scene`) made its compile miss the blob cache.
The same ctypes driver, at HEAD and on the working tree, gave the same failure; leaving `opacity_micromap` out compiled in 17 s.
A run the watchdog does not stop compiles it in about 340 s and then fails only the vulkan agreement test.

## Fix

Either half is enough to trigger it: with no query flagged, the capability alone still took DXC about 250 s and failed validation.
A vulkan query now carries the flag only where its flags force 2 states or are no constant, and the entry point declares the capability only beside such a query (EMIT-184).
sv's queries all looked non-constant, since a mixed world's trace hands its flags on through a local the prelude names `given`.
The plan now reads a local given a constant and never assigned again as that constant (EMIT-159), so their flags are known and force nothing.
`light_thin` for vulkan compiles in 18 s and validates, and the full `shaped-viewer-test` passes, 553 tests, the vulkan agreement test among them.
Fixed in the working tree on 2026-11-12, uncommitted.
