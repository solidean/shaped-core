# The wavefront path tracer

**Built, and the default.**
The megakernel in [tracer_pipeline.sgl](../shaders/tracer_pipeline.sgl), one ray-tracing pipeline whose raygen walks every path to its end in registers, stays as `render_mode::megakernel`.
This document is the wavefront's shape, the decisions behind it, the alternatives that were weighed and rejected, and what it measured.

## Why

A megakernel pixel is not done until its deepest path is, so one deep corner of a scene sets the cost of the whole dispatch.
The same property is what lets a heavy dispatch trip a driver watchdog rather than merely run slowly.
And neighboring threads that hit different materials run different shading code, which the GPU serializes.

The wavefront moves the path loop to the host.
Each stage is its own dispatch over the paths still alive, so a late bounce costs only its survivors, and shading runs one material at a time.

What it is for, in order:

- **A frame paced by its live paths**, with no dispatch bounded by the worst path in it.
- **Material coherence**: each material's shading runs over only the paths that hit it.
- **Paths that outlive a frame**, so a still view converges on a fixed per-frame budget.
- **dx12, vulkan and metal.** Metal has never run the current tracer, and running there is part of done.

**webgpu is out of scope.**
Inline ray queries would reach it, but it has no bindless binding arrays, and sv's material model is built on them.

## The shape

```
generate ─▶ extend ─▶ sort by permutation ─▶ shade (one dispatch per permutation) ─▶ connect ─▶ resolve
   ▲                                              │
   └──────── a slot whose path ended ◀────────────┘
```

- **generate** writes a pixel's next camera ray into a path slot.
- **extend** traces each live path's next segment inline and records the nearest hit.
- **sort** counts hits per material permutation, turns the counts into offsets and indirect-dispatch arguments, and scatters each slot into its permutation's range.
- **shade** evaluates the material, samples the BSDF for the next direction, and writes the next-event estimation shadow rays with their unoccluded contribution.
- **connect** traces the shadow rays, any-hit only, and adds the contribution of the ones nothing blocks.
- **resolve** folds each pixel's finished paths into the image.

A **round** is one pass of extend, sort, shade and connect: one segment of every live path.
A frame runs a fixed budget of rounds.

## Decisions

### Tracing is inline, in compute

Extend and connect are `@compute` kernels calling `world.trace` ([raytracing.md](../../shaped-graphics-language/docs/spec/raytracing.md#inline-traces)).
A compute kernel has atomics, so it appends to the next queue itself; SGL allows atomics in pixel and compute stages only.
It needs no shader table and no pipeline per set of permutations, and every stage is the same kind of kernel.

**The escape, if it is ever needed, is a ray-tracing pipeline per stage**, with SGL allowing atomics in a raygen behind `raytracing_pipeline`.
The trigger is a cutout-heavy scene where extend measures slower than the pipeline on dx12.

### Cutout is data, not code

**Landed, in the megakernel too.**
Each instance record carries where its opacity lives in the parameter block, and its cutoff (`sv::material_opacity`).
The opacity is a constant, a mesh attribute, or a texture channel read through a uv attribute and a palette sampler.
One fixed function, `tracer.instance_opacity`, reads it inside traversal, so extend and connect never recompile for a material, and every cutout candidate runs the same code.

- A cutoff of 0 is stochastic coverage, as before; above 0 it is glTF's alpha cutoff.
- A type declares `opacity_attribute` and `opacity_cutoff_attribute`; a fragment that writes `geometry_opacity` asserts.
- Both built-in types fit unchanged; a type with a procedural mask can no longer cut out.
- Quadrics cannot cut out today, so cutout stays triangle-only.

**Opacity micromaps are the planned extension** for vegetation-heavy scenes, behind an sg feature, baked from the opacity record.
Only data-driven opacity can feed them, which is part of why the record was chosen over a generated branch per material.

### Shading is sorted, one dispatch per permutation

A permutation is a material's shape — its type, and for each input whether it is a constant, a texture or a mesh attribute.
SGL has no indirect calls, so a kernel reaches only the materials it was compiled with.

After extend, a counting sort by permutation writes one index list with a range per permutation.
Shade is then one indirect dispatch per permutation over its range, so every subgroup runs one material.
A new material compiles one small shade kernel and nothing else.

### sg grows indirect dispatch

`command_list_compute_scope::dispatch_groups_indirect(args, offset)` reads three `u32` group counts from a GPU buffer.

- dx12 is `ExecuteIndirect` with a dispatch command signature, created once per device.
- vulkan is `vkCmdDispatchIndirect`, core 1.0.
- metal is Metal 4's `MTL4::ComputeCommandEncoder::dispatchThreadgroups` with the arguments' GPU address.
- webgpu is `dispatchWorkgroupsIndirect`, in scope for sg even though the tracer is not.

Every one takes a 4-byte-aligned offset.
The barrier backends already translate `access_flag::indirect_read`; what is new is barrier inference recording the args buffer as that read.

### A path slot belongs to one pixel, and is refilled in place

Slot `(pixel, j)` for `j < k`, with `k` from a memory budget and 1 by default.
When a slot's path ends, it adds its radiance to the slot's own float sum and count, and generate writes that same pixel's next sample into it in the next round.

- Every accumulation has one writer, so nothing needs float atomics, which SGL does not have.
- Queues stay full for the whole frame, with no sample-pass boundary to wait at.
- A pixel whose paths are deep finishes fewer samples per frame than a shallow one.

The state is structure-of-arrays, one buffer per field group, so a stage reads only what it uses.
**The path state is stored unpacked, behind accessors**, until per-stage timings show the bandwidth worth packing; ray origins stay 32-bit float whatever is packed.
ReSTIR's records are packed already, which [plans/restir-gi.md](plans/restir-gi.md) describes.

At 1920×1080 with `k = 1`, unpacked, a frame holds roughly 250 MB of path state, 50 MB of hit records, 200 MB of shadow rays and a few index buffers of 8 MB each.

### The image is a per-pixel sum and count

The accumulated target holds a float radiance sum and a count of finished paths per pixel, and the displayed image is their quotient.

**That mean is consistent, not exact.**
The paths still in flight are disproportionately the long ones, so the finished set leans toward short paths by roughly `k / count`: visible in the first frames, gone after a few hundred.

**A drain gives the exact mean**: rounds run without new samples until every queue is empty.
Tests and captures drain before they read.
**A frame that sees the camera or the scene change drains too**, so a moving image is always exact; carrying resumes once it is still.
Every path carries the generation of the camera it started under, and an older one never reaches the image.

`frame_output`, for the temporal denoisers, is this frame's finished paths over their own count, which is noisier than today's, since a pixel can finish none in a frame.
The denoiser learns how converged each pixel is from a per-pixel count texture, an optional input of `sr::reconstruct_inputs`, rather than from one scalar for the image.

### Surface paths end by Russian roulette under a cap

After its third bounce, a path survives with its throughput's largest channel as probability, floored at 0.05, and is divided by it — the rule the medium walk already uses.
`max_bounces` stays a cap, `render_settings`' default of 5, which is what bounds a draining frame.
The last bounce keeps its rule that next-event estimation takes a light's full weight.
At that default, roulette runs once, after the third bounce, and the cap ends every path it spares.
Surface bounces and medium scatters keep separate counters and separate roulettes, so a fog bank cannot exhaust the surface budget.

### A medium's scatter is a segment like any other

Inside a scattering medium, each scatter writes a new ray back to extend, exactly as a bounce does.
A walk that outlasts the frame's rounds continues next frame, so no dispatch is bounded by the walk's length.

### Shadow rays have a stage of their own

Shade computes everything about a light sample but its visibility, and connect traces the batch.
The per-material shade kernels then contain no traversal at all.
Connect adds into a radiance field of its own per slot, which resolve sums with shade's, so the two never write one value in the same round.

### Settings

`render_settings::samples_per_pixel` is replaced by a round budget per frame, and `k` stays internal.
`max_bounces` keeps its name and becomes the cap.

### A still frame fits its budget by rounds, then by fill

A still frame runs as many rounds as `render_settings::frame_budget_ms` holds, from the GPU time its earlier frames measured.
A round of a full slot pool can cost more than the whole budget: ~28 ms of Sponza at 1280x720 on a laptop RTX 5070, against 16 ms.
There the frame runs one round, and a free slot starts a new sample only with probability `fill`, drawn from its own random stream.

- A round then holds fewer paths, so it costs less, and `impl::wavefront_pacing` moves `fill` toward the budget with each measured frame.
- Each pixel still averages whole samples, and which slots start is independent of what any sample returns, so the mean is unchanged.
- A round's cost grows slower than `fill`, since the paths a round carries stay, so each correction is partial and the fill settles over a few frames.
- A restart's first one-round still frames steer nothing, since the drain left their slots empty and they cost less than they will.
- A fill moving toward a target within 5% of 1 snaps to 1 once it is that close itself, so a view turned toward something cheap regains whole rounds.
- Below a full fill a round's queue is never full, so it loses the pixel-order fast path of `round_slot`, and a segment costs more than at full occupancy.

The depth and the hit-distance guides carry over a still frame, since few of its slots start a primary ray and the camera has not moved.
The depth is kept as view depth and made clip depth at resolve, since a drawing that starts needing it sets the near plane without a restart.
`sv.wavefront.still_rounds` and `sv.wavefront.still_fill` record what each still frame ran.

### Where the generic kernels live

The queue append, the count-to-arguments kernel and the counting sort know nothing about paths.
They start in sv and move to shaped-rendering once a second caller exists.

## Rejected

- **Tiling the megakernel's dispatch.** It bounds each dispatch, so it answers the watchdog, and nothing else: a frame still costs the deepest path in each tile.
- **Shader execution reordering on the megakernel.** A hint a driver may ignore, available on some hardware only, and it keeps the megakernel's register pressure.
  It remains worth having inside a pipeline, if the escape above is ever taken.
- **A compute megakernel with inline traces, first.** Its one unique gain was reaching webgpu, which is out of scope.
- **A pool refilled from a global sample counter.** The best utilization, but two slots can then hold samples of one pixel, which needs float atomics or fixed-point accumulation.
  Refilling in place keeps the utilization without either.
- **Folding partial radiance into the image every frame.** The image would no longer be a mean of anything.
- **Discarding in-flight paths on a camera move.** Bounded moving frames, but only paths short enough to finish in one frame would count, so a moving image is biased dark.
  It stays the fallback if a draining frame's cost ever matters more than that bias.
- **An inline walk kernel for media.** It confines the deep-path cost to media rather than removing it, and once paths carry across frames a scatter as a segment costs nothing new.
- **Keeping the megakernel beside the wavefront.** Every estimator change would land twice and drift; the GPU tests' physical expectations are the reference that lasts.

## Measuring

- GPU timestamps per stage, recorded into `cc::rec`.
- Per-round live-path counts, read back for a caller that passes `wavefront_trace_desc::live_paths`.
- One benchmark, megakernel against wavefront, on `openpbr-spheres`, `lights`, `quadric-gallery`, `mesh-structure-dense` and the furnace test's dense interior.

A wavefront is not faster on every scene: on a shallow, single-material one, the stored state's memory traffic usually loses to the megakernel's registers.

### What it measured

`wavefront_routine` records each stage's GPU seconds per frame as `sv.wavefront.*` stats.
A caller passing `wavefront_trace_desc::timing` gets the same timestamps back, and one passing `live_paths` gets each round's live-path count.
`wavefront-benchmark.cc` (`uv run dev.py benchmark "wavefront against megakernel"`) measures GPU time per finished path at 512x512, 24 frames each, with a per-stage breakdown.
**Its contenders run interleaved frame by frame**, because a laptop GPU's clocks drift under sustained load and whatever ran later in a sequence came out up to 50% slower.
On an RTX 5070 Laptop GPU with driver 617.14, in pixel order, on an otherwise idle machine, in October 2026:

| scene | megakernel | wavefront, 8 rounds | wavefront, 16 rounds |
|---|---|---|---|
| Cornell box, 5 bounces | 9.0 ns | 19.0 ns | 18.5 ns |
| dense scattering cube, depth 0.05 | 146 ns | 8.6 ns | 8.0 ns |

The absolute numbers move between runs and drivers, so the ratios are what to read.
On the box, the megakernel is about twice as fast; in the medium, the wavefront is 17 to 18 times faster.
A run on the same machine under load gave the same ratios.
The wavefront's paths end by roulette after the third bounce and the megakernel's do not, which flatters it on the box.
In the medium a megakernel thread waits for the longest walk in its wave, and a wavefront round never does.

**Where the box's time goes**: shade 35%, segment 23%, finalize 20%, connect 10%, extend (the trace itself) 4%.
Two causes were found, and one is fixed:

- `segment` wrote back every record it read, the ray and the light record included, on a path that changed neither; it now writes only what changed.
  The tally is still read and written whole, because SGL cannot assign one member of a buffer element yet.
- **Every queue is filled by atomic append, so a round's order is whatever order threads finished in**, and a wave reads 32 unrelated pixels' state.
  A full queue is a permutation of every slot, so in a full round `round_slot` takes the slot equal to the thread's index, which is exact.
  A wave then reads adjacent state, and its new samples leave from adjacent pixels; it took the box's frame from 550 ms to 400 ms.

### Pixel order and NVIDIA's dx12 driver

With pixel order, an RTX 5070 Laptop GPU on dx12 corrupts the queue counters, on drivers 616.92 and 617.14.
A counter ends a round overwritten with a float bit pattern, sometimes with the round's correct appends added on top, or off by some count.
A dispatch sized from it then hangs the device, or reads never-written queue entries and finalizes slot 0 hundreds of thousands of times.
Everything else runs the same code correctly:

- the same GPU through vulkan, with synchronization validation on and clean (`sv vulkan - the wavefront raises no synchronization hazard`);
- the Radeon 780M through the same dx12 backend and the same DXIL;
- WARP.

No shader instruction can produce the value: every counter access is an add of one, an exchange with zero or an or with zero.
Stronger barriers do not help, and changes that only move timing hide it (GPU-based validation, a copy after every dispatch).
What decides it is the code's shape.
A standalone D3D12 reproduction fails until either of two changes:

- each counter is appended to from one place;
- the round's input counter is read at a computed index rather than from two constant addresses chosen by a branch.

The tracer does both: `segment` routes a path first and appends once per queue, and `input_counter` / `output_counter` compute the index.
With both, pixel order passed 20 repeats of the routine-level tests and 5 of the whole dx12 suite, where it used to hang in most runs.
It is a workaround rather than a fix: it rests on code shape, so a refactor or a new driver can bring the failure back.
`groups_for` clamps every dispatch to the slot count, so a corrupt count costs an image rather than the device.
A standalone reproduction for a report to NVIDIA exists outside the repository.
It reduces to nine lines of HLSL: `InterlockedOr(x, 0)` in each branch of an if/else, at constant addresses, writes `0xffffffff` to a word the shader never addresses.

On the Radeon 780M, where pixel order always worked, the wavefront measures:

| scene | megakernel | wavefront, 8 rounds | wavefront, 16 rounds |
|---|---|---|---|
| Cornell box, 5 bounces | 235 ns | 88 ns | 86 ns |
| dense scattering cube, depth 0.05 | 1716 ns | 46 ns | 44 ns |

## Landing order

1. The accumulated mean is pinned by a GPU readback, `accumulated-mean-test.cc`, against today's tracer.
2. sg: `dispatch_groups_indirect` on every backend, its barrier inference, and opacity micromaps in sg's TODO.
3. The generator emits a plain shade function per permutation, and the instance record gains the opacity record.
4. sr: the optional per-pixel count texture in `sr::reconstruct_inputs`.
5. The wavefront itself: slots, stages, sort, the sum-and-count accumulator, drain, generations.
6. Parity: analytic-light and environment MIS, decals, quadrics, media, the guides, the split signal, motion and primary depth.
7. Measurement, the benchmark and a one-off image comparison against the megakernel.
8. The wavefront becomes the default, and the megakernel stays as a choice — `render_settings::mode`.

The plan ended with deleting the megakernel.
It stays because neither tracer wins everywhere: the megakernel is about twice as fast on a shallow scene, and the wavefront far ahead on a deep one.

### Where it stands

Every step has landed: the wavefront traces a layer unless `render_settings::mode` asks for the megakernel.
The path state lives on the layer's accumulator slot, so the two restart together and the store's budget counts both.
A frame whose image changed restarts and drains, a still one carries for what its frame budget holds, and a layer that denoises hands the spatial member the tallies' per-pixel counts.
Parity is pinned twice: `wavefront-trace-test.cc` against the megakernel on the Cornell box, media, glass and cutouts,
and `wavefront-view-test.cc` through the viewer on quadrics and decals.

What is still open:

- **A draining frame is bounded rather than empty.**
  It runs `max_bounces + 17` rounds, enough for every surface bounce and for a medium walk up to where its roulette starts.
  A deeper walk finishes on the next still frame, or is dropped by the next restart.
- **A moving frame is one sample per pixel**, where the megakernel's is `samples_per_pixel`; starting several per pixel on a draining frame is the fix, if a moving image turns out too noisy.
- **A capture does not drain before it reads.** Its mean is consistent but holds the paths that finished, which favors short ones until the carry catches up.
