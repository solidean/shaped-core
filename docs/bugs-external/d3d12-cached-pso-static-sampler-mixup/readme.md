# D3D12: a cached PSO restored with another pipeline's static sampler

**Status:** open, not yet filed upstream; worked around in shaped-graphics.
**Affects:** NVIDIA D3D12 driver on Windows 11, RTX 5070 Ti, driver 32.0.15.9186; WARP is not affected.
**Found by:** `shaped-graphics-test`'s file-scope sampler test failing on dx12 only, depending on the order tests ran in, and only with sg's persistent pipeline cache warm.

## What happens

One compute shader is built under two root signatures that differ only in one static sampler: one clamps, one repeats.
A process builds the clamping pipeline fresh and stores its `GetCachedBlob`.
A later process builds the repeating pipeline fresh, then builds the clamping one from the stored blob.

`CreateComputePipelineState` accepts the blob and reports no error.
The pipeline it returns samples through the *repeating* sampler, though its root signature and its blob are both the clamping one's.

Restoring the same blob in a process that has not built the repeating pipeline is correct.
So is restoring it after building the same pipeline fresh.
The blobs of the two pipelines differ, so the sampler's state is in the blob: it is the restore that ignores it.

A fresh build that happens in the same bad state can write such a blob too.
sg's persistent pipeline tier was found holding a clamping pipeline's entry that sampled by repeat on every restore.

## Why it is the driver and not us

`repro.cc` is D3D12, DXGI and the system shader compiler (FXC, `cs_5_1`), plus the C++ standard library.
Each row below is its own process, run in order, after a process that stored the clamping pipeline's blob:

```
adapter                    restore                        result
-------------------------  -----------------------------  ------------------------------------
RTX 5070 Ti, 32.0.15.9186  alone                          correct
RTX 5070 Ti, 32.0.15.9186  after a clamping twin, fresh   correct
RTX 5070 Ti, 32.0.15.9186  after a repeating twin, fresh  WRONG: every clamped texel repeats
WARP 10.0.26100.9278       every row above                correct
```

The load-bearing parts:
- **Two processes.** A process that builds the clamping pipeline itself, before or after the twin, restores the blob correctly, so a single-process test cannot show it.
- **A static sampler.** A pipeline whose root signature carries none restores correctly under the same sequence in shaped-core's own tests.
- **The twin's shader is the same one.** Its root signature differs only in the sampler's address mode.

## Reproducing it

```bash
uv run run.py
```

It builds `repro.cc` with `clang-cl`, runs the sequence on the high-performance adapter and on WARP, and exits 1 when a restore samples wrongly.

## What shaped-core does about it

A dx12 pipeline whose root signature carries a static sampler, a group's or the layout's own, neither restores from a cached blob nor hands one out.
The rule is `dx12_pipeline_layout::has_static_samplers`.
[dx12_pipeline_layout.hh](../../../libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.hh) states it.
[compute-texture-test.cc](../../../libs/graphics/shaped-graphics/tests/compute/compute-texture-test.cc) pins it, since the fault itself needs two processes.
Every other pipeline keeps its persistent cache.

## Re-checking after a driver update

Run `uv run run.py`.
If it reports the fault gone, drop `has_static_samplers` from the dx12 backend and its test, and this directory with them.
