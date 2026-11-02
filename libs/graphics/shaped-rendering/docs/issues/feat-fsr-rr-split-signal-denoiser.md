---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/docs/reconstruction.md
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/reconstruct.cc
  - symbol: sr::denoise_method::fsr_rr
---

# `fsr_rr`: the FidelityFX split-signal denoiser as a member

`denoise_method::fsr_rr` is named, carries its guide contract, and has no routine: `reconstruct_routine::execute` forwards it nowhere.
[reconstruction.md](../reconstruction.md#the-members) is the design.

The member is re-scoped from "Ray Regeneration" to FidelityFX SDK 2.3.0's `ffx_denoiser.h`.
That one is split-signal: separate dispatches for direct and indirect diffuse and specular, ambient occlusion and specular occlusion.
So it is temporal, does not upscale, and requires `split_diffuse_specular` and `hit_distance` on top of what `dlss_rr` needs — the same guide contract NRD wants.
NRD is the reference it will be judged against.
What it waits for is the tracer rather than the SDK.

The rest of the original denoising item has landed: sg's declared native scope (`dx12_native_scope`), `dlss_rr` over it, and `nrd`.
