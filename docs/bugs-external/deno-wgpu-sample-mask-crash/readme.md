# wgpu: a fragment shader writing `@builtin(sample_mask)` crashes the process on vulkan

**Status:** not filed; nothing in the tree works around it.
**Affects:** Deno 2.9.7's wgpu on its vulkan backend, on an Intel Arc B580 (driver 101.9150) under Windows 11.
**Found by:** `shaped-graphics-test` dying under `--runtime deno` with exit 0xC0000005, on `sg - a sample mask the pixel stage writes keeps the samples it leaves out`.

## What happens

`createRenderPipelineAsync` over a fragment stage that writes `@builtin(sample_mask)` ends the process with an access violation.
No JavaScript error is raised, and nothing is logged.

## Why it is not our bug

`repro.mjs` touches nothing of ours: it draws one 4-sample pixel through `navigator.gpu`, with WGSL it states itself.

| Fragment stage | deno, vulkan | deno, dx12 |
|---|---|---|
| no `sample_mask` | draws, samples `[1, 1, 1, 1]` | not run |
| writes `sample_mask = 5` | **0xC0000005 at pipeline creation** | draws, samples `[1, 0, 1, 0]` |

The WGSL is valid, since the dx12 row runs it and gets the right samples.
sg's own vulkan backend passes the same test on the same device through DXC's SPIR-V, so the device has the feature.
That leaves wgpu's vulkan path, or how the Arc's driver takes what wgpu hands it; nothing here separates those two.
`WGPU_ADAPTER_NAME` did not move deno to the AMD iGPU, so no other vendor has been tried.

## Reproducing

```bash
uv run run.py
```

## What we do about it

Nothing yet: deno picks vulkan here by default, so `shaped-graphics-test` under `--runtime deno` cannot finish on this machine.
[bug-sg-deno-wgpu-suite-segfault](../../../libs/graphics/shaped-graphics/docs/issues/bug-sg-deno-wgpu-suite-segfault.md) tracks that.

## What to re-check on an update

Run `run.py` after a Deno or an Intel driver update, and try another vendor's GPU, which decides whose bug this is.
