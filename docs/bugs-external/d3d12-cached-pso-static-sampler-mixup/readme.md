# D3D12: a cached PSO restored with another pipeline's static sampler

**Status:** open, not yet filed upstream; worked around in shaped-graphics.
**Affects:** NVIDIA D3D12 driver on Windows 11: RTX 5070 Ti with driver 32.0.15.9186, and RTX 4090 with driver 32.0.16.1088 (2026-11-14); WARP is not affected.
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
RTX 4090, 32.0.16.1088     alone                          correct
RTX 4090, 32.0.16.1088     after a clamping twin, fresh   correct
RTX 4090, 32.0.16.1088     after a repeating twin, fresh  WRONG: 192 wrong texels
WARP 10.0.26100.9278       every row above                correct
WARP 10.0.26100.9818       every row above                correct
```

What the bug needs:
- **Two processes.** A process that built the clamping pipeline fresh before restoring its blob restores it correctly, so a single-process test cannot show it.
- **A static sampler.** Observed in sg rather than shown by this repro: a pipeline whose root signature carries none restored correctly under the same sequence there.
- **The twin's shader is the same one.** Its root signature differs only in the sampler's address mode.

## Reproducing it

```bash
uv run run.py
```

It builds `repro.cc` with `clang-cl`, runs the sequence on the high-performance adapter and on WARP, and exits 1 when a restore samples wrongly.
It exits 2 when the run is inconclusive, which includes a restore the driver built without the blob.

## What shaped-core does about it

The dx12 backend makes no static samplers at all.
Each one becomes a one-descriptor sampler table, pointed at its state's slot in the sampler heap when a pipeline is bound.
The slot is interned once per state and never freed.
That is `dx12_sampler_interner`, in [dx12_sampler.hh](../../../libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_sampler.hh).
Two variants of one shader that differ in a sampler then share a root signature, so there is no twin for the driver to confuse, and their blobs persist.

`run.py` checks that too: `repro --table` runs the same sequence with the sampler bound through a table.

```
adapter                    restore                        result
-------------------------  -----------------------------  ------------------------------------
RTX 4090, 32.0.16.1088     after a repeating twin, table  correct (2026-11-14)
```

Where the tables would not fit, past the 64-DWORD root budget or the sampler heap, a layout falls back to true static samplers.
Such a layout neither restores from a cached blob nor hands one out: `dx12_pipeline_layout::has_static_samplers`.
[compute-texture-test.cc](../../../libs/graphics/shaped-graphics/tests/compute/compute-texture-test.cc) pins that a table-bound pipeline restores from its blob and keeps its own sampler.

## Re-checking after a driver update

Run `uv run run.py`.
It exits 1 while the static-sampler fault is there, 4 if the table route goes wrong too, and 0 when neither does.
If the fault is gone, the fallback's blob rule can go; the tables can stay, since they cost one root DWORD each.
