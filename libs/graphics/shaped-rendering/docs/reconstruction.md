# Reconstruction

One call turns a path-traced frame into the image a caller shows: it denoises, upscales behind the denoiser, and will generate frames.
`sr::reconstruct_routine` is the front: a caller names a denoiser and an upscaler, or `automatic`, and the front forwards to the member routines that implement them.
Every member is a routine of its own too, callable directly with its full options.

This is the design, including the parts not built yet.
[reconstruct.hh](../src/shaped-rendering/reconstruct.hh) is the API, and [structure.md](structure.md#reconstruction-in-progress) says what exists.

## The members

| member | kind | where it runs | status |
|---|---|---|---|
| `atrous` | spatial | dx12, vulkan (HLSL through DXC); WARP included | done |
| `svgf` | temporal | dx12, vulkan (HLSL through DXC) | done |
| `oidn` | spatial, trained | dx12, vulkan (HLSL through DXC) | done; named only, not real-time |
| `dlss_rr` | temporal, upscales | NVIDIA RTX; dx12, vulkan | planned |
| `fsr_rr` | temporal, upscales | AMD RDNA 4 (Ray Regeneration); dx12 | planned |
| `nrd` | temporal, split-signal | dx12 (DXIL); WARP included | done, sources fetched on request |

| upscaler | kind | where it runs | status |
|---|---|---|---|
| `fsr` | temporal, upscales, does not denoise | any GPU; dx12 and vulkan on Windows builds; not WARP | done |

**A spatial member reads one image; a temporal one also reads history reprojected by motion vectors.**
The temporal ones work from about one sample per pixel, but only if every pixel's motion is known.

**An upscaler is not a denoise member; it runs behind one.**
DLSS Super Resolution, FSR 3.1 and FSR 4 are temporal upscalers, and on path-tracing noise they smear it rather than remove it.
AMD's own guidance places denoisers before the upscaler, at the traced resolution, and that is the order the front runs them in.
The vendor products that denoise are Ray Reconstruction and Ray Regeneration, and both upscale as part of it, so no upscaler runs behind them.

**The native members are what CI tests, and `nrd` does not join them.**
à-trous and SVGF are our own HLSL, so they run on WARP, and the front's policy is tested through them.
CI never fetches NRD, so what it builds and runs is the null path: `SR_HAS_NRD` is 0, the member reports `unsupported`, and its own tests do not exist.
They run only where somebody fetched the SDK, which is why a green CI here says nothing about whether NRD works.

**NRD is a planner rather than a renderer, which is why it asks nothing of the adapter.**
It compiles nothing at run time, owns no device memory and records nothing.
What it answers is "which compute dispatches would denoise this frame, against which resources, with which constants", and sr executes that answer through sg.
So it needs no native scope and no vendor runtime: it runs on whatever adapter dx12 gives it, WARP included, which is what lets it be tested without the hardware that shipped it.
`fsr_rr` is expected to want the same split signal once it exists, which is why NRD is the reference it will be judged against.

**It is dx12-only today, and that is a scope call rather than a property of NRD.**
The member builds only where a pinned `dxc.exe` compiles NRD's own shaders, which is Windows.
It embeds DXIL alone, so `nrd_session::create` hands sg `sg::shader_format::dxil` and a vulkan context would refuse the bytecode.
Two routes widen it, and the cheap-looking one was tried and is not cheap.

**`nrd::PipelineDesc` carries a `computeShaderSPIRV` beside its DXIL, and turning it on is genuinely one line.**
The DXC `extern/dxc` already pins emits SPIR-V, so `NRD_EMBEDS_SPIRV_SHADERS ON` plus `SHADERMAKE_DXC_VK_PATH` produces 31 SPIR-V blobs with no second compiler and no network.
What does not follow is a working vulkan arm.
Built that far and run against a validating vulkan context, two things break, and neither is a line of configuration.

- **NRD uses two register spaces, which are two descriptor sets under Vulkan.**
  `nrd_session::create` builds one binding group, which is one set, so the constants land outside the layout it declares:
  `vkCreateComputePipelines(): ... uses descriptor [Set 1, Binding 2, variable "REBLUR_SplitScreenConstants"] but the binding was not declared in the VkPipelineLayoutCreateInfo::pSetLayouts[1]`.
  dx12 does not care, which is why the DXIL path never noticed.
- **NRD's shaders want compute derivatives**, and sg's vulkan backend enables no such feature:
  `SPIR-V Capability ComputeDerivativeGroupQuadsKHR was declared, but ... computeDerivativeGroupQuads` is required.
  So is the `VK_KHR_compute_shader_derivatives` extension, which is the same gap seen from the instance side.

Both are answerable — a second binding group here, a device feature in sg — and neither is in this change.
**With validation off the same run passes**, which is worth knowing before anyone reads a green run as support.

Beyond that, NRD ships its shaders as source, and `PipelineDesc::shaderIdentifier` exists so a custom integration can supply its own compiled form.
An SGL port of them would reach webgpu and metal too.
What it costs is owning a translation of someone else's tuned numerics, and keeping that translation agreeing with a constant layout NRD still lays out.

**Its encodings are exact formulas, not conventions, and we never reimplement them.**
NRD does not take a normal, a roughness and a hit distance as such.
It takes one normal-roughness texture in an encoding its own build chose, and radiance in YCoCg whose alpha carries a hit distance normalized against a curve of view depth and roughness.
So `nrd_repack.hlsl` and `nrd_resolve.hlsl` include NRD's own `NRD.hlsli`.
[extern/nrd/CMakeLists.txt](../../../../extern/nrd/CMakeLists.txt) copies it into sr's shader directory at configure time, because a shader package resolves every include under one source directory.
A reimplementation that drifted would be a worse image rather than a build error.
`nrd_session::create` refuses outright a library whose reported encodings are not the ones the repack target's format assumes.

**The radiance handed over is de-modulated, which is what keeps a surface's texture from being filtered as noise.**
NRD's input contract asks that radiance carry no material information, and `NRD_MaterialFactors` is the helper it ships for the purpose.
So that is what the repack divides by and the resolve multiplies back.
The factors are written to scratch by the repack rather than recomputed by the resolve, because NRD requires both directions to use the same ones.
Storing them makes that structural, instead of two passes independently agreeing on a camera, a normal and a roughness.
That is why `albedo` and `specular_albedo` are REQUIRED guides for this member rather than optional ones.

It is partial by construction, because NRD floors both factors well above zero and calls the specular half a biased solution.
On a checkerboard albedo under one flat normal, the case where nothing but the albedo says there is an edge, the member keeps about nine tenths of the contrast.
Feeding radiance straight through keeps under one tenth of it.

**Known limit: a lobe no path sampled reaches REBLUR as contact rather than as absent.**
One path carries one lobe, so a pixel whose paths all went diffuse has no specular hit distance, and the tracer reports 0 — which is NRD's own "this lobe was not sampled here".
`nrd_repack.hlsl` then passes it through `REBLUR_FrontEnd_GetNormHitDist`, whose `max(hitDist, NRD_EPS)` turns that 0 into the smallest non-zero distance.
REBLUR reads a distance that small as a reflection of something touching the surface.
Closing it takes three things together, per `NRDSettings.h`'s `HitDistanceReconstructionMode`.
The primary hit's diffuse/specular choice clamped to [1/4, 3/4] and drawn with Bayer dithering rather than white noise, so every 3x3 area holds a sample of each lobe.
A raw 0 passed through the repack as 0 rather than floored.
And `reblur.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3`.
That is a tracer change of its own, so it is recorded here rather than done.

An escaped secondary ray is a different case and is handled.
The tracer counts escapes apart from hits and reports the mean over the paths that hit.
Where every path of a lobe escaped it reports the ray's own `TMax`, which the normalization saturates, so it reads as "far".

**Two conventions run the other way round from ours, and both are carried in settings rather than in a repack.**
NRD reads a motion vector as `pixelUvPrev = pixelUv + mv`, so its units are UV and its direction is previous minus current, where ours is pixels and current minus previous.
`motionVectorScale` carries the reciprocal extent and the sign, so the guide itself is handed over untouched.
Its matrices, despite what `NRDSettings.h` says in prose, are built column by column from the `float[16]`, which is `tg`'s own convention, so they are copied rather than transposed.

## The FSR upscaler

**FSR 3.1's analytic upscaler, which runs on any GPU.**
FSR 2 is no more portable: both are compute shaders on Shader Model 6.2, and only AMD's ML generation — FSR 4, Ray Regeneration, Radiance Cache — needs AMD hardware.
FSR 3.1 is FSR 2 continued with the same inputs, and both sit in one folder of the FidelityFX SDK.

**AMD's host code, our backend.**
The SDK's host C++ decides every frame: which of its passes run, their constants, which of its images each reads and writes, and which get cleared.
It reaches the GPU only through the `FfxInterface` table, and [impl/fsr_backend.cc](../src/shaped-rendering/impl/fsr_backend.cc) fills that table over sg.
Resources become sg images, pipelines become the passes compiled through slib, and scheduled jobs are recorded into the caller's command list, with sg inferring every barrier.
So there is no signed DLL, no native scope, and nothing sg's barrier tracking cannot see.
The public SDK does not carry what the host code includes from `amdinternal/` — a debug watermark and a git hash — so extern/fidelityfx supplies stand-ins that do nothing.

**The passes are ours to compile, in one permutation.**
AMD's precompiled shader headers, and the FidelityFX_SC tool that makes them, are not in the public tree.
Each pass is compiled through slib instead, from a wrapper that states the permutation's defines and includes AMD's pass.
The context flags are fixed — linear HDR in, low-resolution motion, inverted infinite depth — so one variant of each pass is all the host code ever asks for, and the backend refuses any other.
The portable path is the one built: fp32, no forced wave64, no Lanczos lookup table.
The fp16 and wave64 variants AMD tunes for are a follow-up that needs sg to report 16-bit support; [TODO.md](TODO.md) has it.

**The inputs meet FSR's conventions in the backend, and sr's guides stay as they are.**
- Depth: sr's linear view depth is converted to an inverted, infinite-far device depth by a small pass, `near / depth`, which FSR reads back as the same view depth.
- Motion: sr's motion is the surface's own, measured from where this frame's sample was traced, so a still camera reports zero whatever the jitter.
  FSR's runs the way back, so `motionVectorScale` flips the sign.
- Jitter: FSR's offset is the one a rasterizer adds to its projection, the opposite of where the sample lands, so it is negated.
  A test pins the sign: the flipped one reconstructs visibly worse.
- Exposure: `reconstruct_settings::exposure` is the value FSR's exposure image holds.

**One offset per frame.**
An upscaler reconstructs from where each frame's samples landed, so every sample of a frame has to land at the same offset from its pixel's centre.
`sr::reconstruct_jitter` hands out FSR's Halton sequence, and a tracer uses it in place of a random position inside the pixel while an upscaler runs.
It answers (0, 0) when nothing upscales, so a tracer may always ask.

**An upscaled image's alpha is the upscaler's.**
The alpha rule below holds for every denoise member; an upscaled output's alpha cannot be `color`'s, which is at the other extent.

**Its history sits beside the denoiser's.**
`reconstruct_history` holds a second history for the upscaler, and the denoiser's output at the traced extent, so neither one's rebuild drops the other.
A different denoiser is a different image to accumulate, so switching it restarts the upscaler too.
FSR keeps two full images at the output extent and about a dozen at the traced one: roughly 80 MiB for 720p in and 1080p out.

**Where it runs today.**
- It builds where extern/fidelityfx was fetched, which is Windows: the host code calls MSVC's secure C runtime and uses `__declspec` unconditionally.
- It runs on the hardware adapter.
  WARP crashes inside its own shader compiler on FSR's shading-change pyramid pass, and the FSR image tests skip there; [TODO.md](TODO.md) records what bisecting it established.
- It runs on vulkan too, through a patch that gives each of AMD's register classes its own SPIR-V binding range; [TODO.md](TODO.md) has what replaces it.

## The OIDN member

**Intel's weights, our inference.**
OIDN's own GPU devices are CUDA, HIP, SYCL and Metal over vendor GEMM libraries, and would share memory with sg through an OS handle sg cannot export.
Its CPU device would cost a download and an upload every frame.
The weights are a separate Apache-2.0 repository, and they describe a small U-Net: sixteen 3x3 convolutions with a bias and a ReLU, four 2x2 max pools, and four nearest upsamples with a skip concat.
So the member runs that network in five compute shaders of its own, with no vendor SDK and no particular hardware.
They are HLSL today, so it runs where the other native members do, on dx12 and vulkan.

**It is correct and portable, and far too slow for a frame loop** — roughly 0.2 s per megapixel, which is why `automatic` never picks it.
Two networks are fetched, both `rt_hdr_alb_nrm`: HDR radiance with an albedo and a normal, the guides the tracer writes.
The base one is what OIDN's balanced quality runs, and the small one what its fast quality runs: the same topology with every encoder at 32 channels, at half the compute.
`reconstruct_settings::quality` picks between them the same way, `fast` running the small one; there is no large network for these guides.

### How it runs

- **The topology is a table and the widths are data.**
  `impl/oidn_network.cc` lists the layers, and every width is read from the weights file.
  A weights bump then fails to find a tensor, or fails the shape test in `tza-test.cc`, rather than drifting.
- **The weights are read once per process** and packed into one buffer as `[ky][kx][i][o]`.
  The output channel is innermost because it is what varies across a wave, so one weight load touches one or two cache lines.
- **Every channel count is padded to a multiple of four**, so a convolution reads its source four channels per `float4`.
  Only three shapes need it: the nine input channels, the three output ones, and the seventy-three `dec_conv1a` concatenates.
  A padding channel holds a hard zero under a weight of zero, since a NaN left in memory survives a multiply by nothing.
- **A convolution thread produces a run of eight texels.**
  Each weight load is spent on all eight, and each loaded input row on all three kernel columns.
- **Large images run in tiles**, because the network holds twenty-five feature maps at once — the skips stay live across the decoder.
  Run whole that is 2.7 GiB at 1080p and 10.7 GiB at 4K, which `oidn_network::feature_bytes_for` computes.
  - A tile computes an 80-pixel border on every side and discards it.
    80 is where the receptive field ends: tiled and whole agree to a mean below 1e-6 at 80, differ by 1.4e-03 at 64, and by 2.8e-01 with no overlap.
    OIDN derives its own overlap as `round_up(receptiveField / 2, tileAlignment)`, which puts its base model in the same range.
  - Every tile's origin is a multiple of sixteen, the grid four pools need, and an edge tile is shifted inward to end where the whole run's padded tensor ends.
    So a tile sees exactly the input the whole run sees, including the zero padding past the image, as OIDN pads.
  - An axis that fits under the cap stays one span, whatever the other axis needs.
  - Within the cap, `oidn_options::max_tile` with a default of 512, `create` picks per axis the tile that computes the fewest pixels rather than the largest.
    Over 1920x1080 a 512 tile computes more than a 448 one, because its interior divides the image badly.
  - Raising the cap is the one knob that pays: it buys back overlap for memory, per the table below.
    OIDN itself never tiles below 768.
- **Binding groups are built with the network**, since a tile changes push constants and nothing a group names.
  Only the two that name the caller's own textures are made per call, and never per tile.

### Measured

One 1080p frame on the development machine's dx12 GPU, by tile cap; the default cap of 512 chooses a 480x432 tile.

| cap | time | feature maps |
|---|---|---|
| 384 | 500 ms | 197 MiB |
| 512 | 378 ms | 277 MiB |
| 640 | 308 ms | 451 MiB |
| 768 | 276 ms | 602 MiB |

OIDN's own CUDA device filters the same frame in 20.6 ms.
Its CPU device takes 581 ms, and 327 ms with the small network, so the member is faster than Intel's own portable path.
All three were timed alike: device-resident buffers, warmed, best of several, with only the filter and its sync on the clock.
The CPU figure is `uv run dev.py test "OIDN's CPU device timed at 1080p" --manual`, with the library fetched.

- **Cost is flat per computed pixel**, about 80 ms per computed megapixel.
  So the cap trades memory against the overlap computed twice, and never against quality.
- **The gap to CUDA is architectural.**
  OIDN runs cutlass implicit-GEMM convolutions in fp16 on tensor cores, with channels padded to eight, "required by Tensor Core operations".
  Ours is fp32 SIMT at about 3.2 TFLOP/s of a roughly 20-25 peak.
  A perfect fp32 kernel would still land near 60 ms; the rest is matrix hardware, which neither the HLSL we compile nor SGL reaches today.
- **The limit is memory divergence, not issue rate.**
  Reading four channels per load cut load instructions fourfold and bought 1.4x, because a window's eighteen texels lie far apart and a `float4` costs the same cache line as a `float`.
  A layout that keeps a window's texels contiguous, as OIDN's CPU device does with CHW, is what would pay next.
- **Blocking the output channels as well does not pay, under either weight layout.**
  On a 256x256 tile, 16x1 is 8.0 ms where 16x2 is 10.9 and 8x2 is 9.4.
  The wave's lanes already share the input, so a second blocking dimension only costs registers.
- **Half precision would buy memory rather than speed**, since the limit is cache lines rather than bytes, and memory is what buys a larger tile.
  It is optional on every backend, so it wants a feature level; [TODO.md](TODO.md) has what that takes.
- **Once the member runs on WebGPU, its default limits will cap the tile before memory does.**
  `maxStorageBufferBindingSize` defaults to 128 MiB, and the largest binding is one full-resolution map of sixty-four channels.
  That is 50 MiB at the default cap, 110 MiB at 768 and 137 MiB at 1024, so the cap cannot go far past 768 on a device with default limits.
- **The small network** runs the playground's 1600x900 frame at 6.0 fps against the base one's 3.7, tracer included, which is 1.6x of the 2x its compute predicts.
  The table above is the base network's; the small one has no 1080p timing yet.
- Reproducing the CUDA timing means creating `oidn::DeviceType::CUDA` in `tests/oidn_reference.cc`, with `OpenImageDenoise_device_cuda.dll` from the upstream archive beside the core.
  The fetch keeps only the CPU device.

### Getting faster

In order of what each buys for what it costs, with what SGL would have to grow for a port to keep up.
The oracle below is what makes each step cheap to try: a step either keeps the output within its bounds of Intel's, or visibly does not.

1. **Workgroup-memory tiling, implicit-GEMM style.**
   A workgroup stages the input halo and a slab of weights in shared memory, and each thread computes a block of outputs by channels from registers.
   It attacks the measured limit, memory divergence, and a well-tuned fp32 kernel lands near 60 ms at 1080p against 378 today.
   SGL already has both halves it needs: `@workgroup` bindings and `workgroup_barrier()`.
2. **Fusion**: each max pool folded into the convolution before it, and each upsample and concat into the convolution after it, as OIDN does.
   It removes eight passes and their round trips through memory.
3. **Half precision**, behind a feature level, for storage first and arithmetic second; [TODO.md](TODO.md) has what it takes here.
4. **Winograd F(2x2, 3x3)**, since every layer is 3x3: 2.25x fewer multiplies, less after its transforms, and worth doing only after 1.
5. **Matrix hardware**, the remaining ~3x to OIDN's own 20.6 ms.
   Vulkan and Metal expose it today; DirectX's is in preview and WebGPU's experimental, so it waits, and a non-matrix path stays mandatory.

[compute-throughput.md](../../shaped-graphics-language/docs/spec/incubator/compute-throughput.md) records what 3 and 5 ask of SGL.

### Held to Intel's output

`oidn-network-test.cc` runs Intel's own filter over the same input and compares, through `tests/oidn_reference.hh`.
The library it needs is fetched on request, with `uv run extern/oidn/fetch-oidn.py`, and the comparison skips without it.

The scene spans ten decades of radiance over hemisphere-bump normals, at sizes that are not a multiple of sixteen, and every pixel is compared by relative difference.

- Untiled, over 72x72: a mean relative difference of 8.8e-07 and a worst of 1.2e-05.
- The small network, untiled over 72x72 against Intel's fast quality: a mean of 1.1e-06 and a worst of 1.2e-05.
- Tiled, four tiles over 344x344 at a 336 cap, small network against Intel's fast quality: a mean of 1.0e-06 and a worst of 1.6e-05.
- Padding with the image's edge instead of zeros moves the mean to 2.5e-02, and decoding subnormal weights one exponent off moves it to 4.4e-05.
  The bounds, 1e-5 on the mean and 2e-4 on the worst, sit about a decade above the measured values and below both mistakes.

It is the only test that can catch a self-consistent mistake: every other one checks a piece against its own definition.

## The contract

**Reconstruction from day one.**
The output may be larger than the input: a vendor member upscales by itself, and any other member has an upscaler behind it.

- Everything the tracer produces is at the **input** extent, in input pixels: the colour, every guide, motion vectors and jitter.
  Only `reconstruct_inputs::output` is at the output extent.
- A caller never computes a ratio.
  It picks a `render_scale_preset` and asks `sr::reconstruct_input_extent` what to trace.
  A denoiser with no upscaler behind it answers every preset with the output's own extent, so a caller cannot ask for a ratio a member would reject.
  So does an upscaler or an upscaling member this device cannot run: the call is about to be refused, and a caller that traced smaller for it would composite a smaller image into its own output.
  A free ratio can join later as one more way to ask.
- A change of either extent restarts the history, like a resize.

**Guides are declared by the member.**
`sr::required_guides(m)` is what a member cannot run without, and a call missing one reports `unsupported`.
`sr::optional_guides(m)` is what it uses when present.
A tracer writes the union for the members it may hand off between, in a few fixed tiers rather than one permutation per combination.

**`albedo` and `specular_albedo` are the two halves of one surface's reflectance.**
`albedo` is diffuse only, so it is zero on a metal, whose colour is all in `specular_albedo`.
A member reading split radiance reads the two separately, one per half.
A member filtering unsplit radiance — à-trous and SVGF — demodulates by their sum, or a textured metal's base colour is filtered as noise.
OIDN is the follow-up: its own documentation wants a metal's albedo to be its specular colour and glass's to be about 1, and `oidn_network::execute` takes only `albedo` today.

**Settings are one flat struct of knobs named for what they do.**
Each field in `sr::reconstruct_settings` says which members read it, and a member ignores the rest, so switching members keeps every knob that still means something.
A member's own options — the full vendor surface — live on the member, never in the shared struct.

**Whether a call carries fresh samples is one of those knobs, not an argument.**
`reconstruct_settings::fresh_samples` is what tells `automatic` to pick among the temporal members.
`sr::reconstruct_input_extent`, `sr::resolve_denoise_method` and `sr::reconstruct_routine::execute` all read that one answer.
It sits in the struct rather than beside each call because planning a frame and running it are three calls apart.
A caller that said yes to one and nothing to another would have traced for a member the call then does not use.

**A denoised image keeps the alpha it came in with.**
Every denoise member copies `reconstruct_inputs::color`'s alpha into `output` and writes only rgb, so a caller compositing with alpha gets the same channel whichever member ran.
SVGF carries a per-pixel variance in alpha between its own passes and swaps it for the caller's on the last one.
An upscaled output is the written exception: its alpha is the upscaler's, since `color`'s is at the other extent.
Whether a vendor member can honour the rule is open — it may write its own alpha and leave us no say — and that is the point at which the rule is either kept by a copy pass or relaxed in writing.

## Selection and refusal

**Explicit means explicit.**
Naming a member this build or device cannot run reports `unsupported`, logs once per process on sr's domain, and writes nothing.
Only `automatic` chooses, walking the members best first:
`dlss_rr`, `fsr_rr`, `nrd`, `svgf`, then `atrous` for a caller feeding fresh frames; `atrous` alone for a caller denoising a converging mean.
`oidn` is never chosen: at roughly 0.2 s per megapixel it is a reference-quality member rather than a frame-loop one, so a caller names it.

The upscaler resolves after the denoiser, and the same way.
`automatic` is `fsr` where it runs and the scale is below native, and `none` otherwise; a named upscaler runs at a native scale too, as anti-aliasing.
A denoiser that upscales by itself has no upscaler behind it, and `denoiser = none` with an upscaler set upscales alone.
An upscaler requires the depth and motion guides, and a call without them reports `unsupported`.

A silent fallback would make a comparison between two named members compare one with itself, which is the failure the framework's three-state readiness exists to prevent.

**Members are acquired when the call runs, never through dependency tokens.**
A token holds its holder pending until its whole subtree is ready, so one member this device cannot initialize would hold every other one hostage.
Instead the front's `init` prewarms every supported member, so prewarming the front still starts their compiles on the next tick.

## History belongs to the caller

`sr::reconstruct_history` is the images a member keeps between calls for one image stream: a temporal member's history, and the scratch a spatial member ping-pongs through.
It is move-only, since a copy would fork a history, and the caller holds one per stream.

**It holds textures, plus one object of the member's own.**
State that is not a texture — OIDN's network today, a vendor member's *feature handle* later — sits in a type-erased `std::shared_ptr<void>`, so `reconstruct.hh` names no member's type.
`_prepare` drops it with the textures whenever the member or the extent changes, and it may hold only what is safe to drop mid-frame, as sg resources are.

**A temporal history is large.**
svgf holds eight full-screen images — six `rgba32_float` and the moments pair `rg32_float` — which is about 221 MiB per 1080p stream and 886 MiB at 2160p.
à-trous holds at most two, and only above two wavelet passes.
That is per stream and on top of whatever the tracer already keeps, so **a caller with several views should drop the history of one nobody is looking at**; dropping it is what frees the images.

A routine is a per-context singleton and cannot know which stream a call belongs to, or when a stream has gone.
The caller can, which is the same reason sv keeps its accumulators in its per-view store rather than in a routine.
`reset()` is a camera cut: the next call starts from nothing and reports `restarted`.

**Two alternatives, rejected — recorded here so they are not re-proposed.**

- **A stream id, with each member routine keeping a map from id to state.**
  Nothing would free a stream, since a routine still cannot see a viewport close.
  Either the caller calls a release it will eventually forget, or every closed view leaks its images until the context dies.
  It also puts a map lookup on the render path, and a lock the moment two views are recorded from two threads.
  The caller-owned object frees itself, which is the whole point.
- **Members registering themselves with the front through an interface, in place of the enum and its switch.**
  It would make adding a member a one-file change, and it would cost the two preference orders their readability — they become data spread across members, or a priority number each one asserts.
  It also turns which members exist into a runtime fact, so the `CC_UNREACHABLE` that catches a member added to the support query and forgotten in the switch has nothing to fire on.
  The member set is enumerated in one enum, at five; a registry is machinery for a larger set than this will ever be.

## Meeting a progressive path tracer

A progressive tracer keeps a running mean that converges while the camera is still and restarts when it moves.
The denoiser attaches to it in two halves, and both are built:

- **Still camera: a spatial member on the mean.**
  The caller passes the mean's total sample count, and the member backs off as it grows.
  à-trous scales its luminance edge-stop by `1 / sqrt(sample_count)`, which is how a Monte Carlo mean's noise shrinks.
  The mean itself is never touched, so the image still converges to the unbiased answer, and turning the denoiser off costs nothing.
- **Moving camera: a temporal member on fresh samples.**
  While the mean is young, the tracer also writes the frame's own samples and motion vectors, and a temporal member denoises those with its history.
  Once the mean holds enough frames — a per-layer threshold — the caller switches to the spatial half.
  The switch is crossfaded rather than cut, because the two produce visibly different images of the same estimate and a jump in an image that is otherwise only getting quieter reads as a glitch.
  Both members run for the fade's length and `sr::mix_routine` blends one into the other, which is what the fade costs.
  The history survives the still period, since the camera it was taken from is the one it is now leaving.
  So the temporal member keeps a history of its own, apart from the spatial one's, or each would drop the other's on every switch.

Feeding a temporal member the mean instead is not an option.
The mean's noise falls every frame while the member assumes it is steady, and its history would double-count what the mean already averaged.

**Two restart signals, not one.**
The mean restarts on any change to what is traced.
The history restarts only when the scene or the shaders change, or on an explicit camera cut, because carrying history across camera motion is exactly what a temporal member is for.
sv takes the scene signal from its trace hash with the camera left out; a caller-facing camera cut is still to come.

## What each member needs from below

- **The native members need nothing sg does not have.**
- **The vendor members need a declared native scope in sg**, per backend, dx12 first.
  Opening it names the resources foreign code will touch and how, so sg emits their barriers and records the declared states.
  It then hands out the native list and resources.
  Closing it only forgets the list's cached bindings, which the caller rebinds before its next draw or dispatch.
  Without it a vendor SDK would bypass sg's barrier tracking silently.
- **OIDN needs nothing sg does not have either**, because the member runs the network rather than the library.
- **Neither does the FSR upscaler**, because sr implements the backend AMD's host code calls.
- **A vendor SDK under NVIDIA's own license is fetched on request, never by default.**
  DLSS sits in sr behind `SR_HAS_<VENDOR>` and links PRIVATE, like SDL3.
  Intel's OIDN library is on request too, for the oracle test alone; its 2.5 MB of weights, two networks, are a default fetch, since the member runs them.
  FSR 3.1's sources are MIT, under 1 MB, and what the upscaler runs, so they are a default fetch as well: a pinned file-by-file subset of AMD's repository, behind `SR_HAS_FSR`.

## Seeing it

`uv run dev.py example shaped-rendering/denoise-playground` is the whole thing on screen.
A small analytic path tracer writes the noisy colour and the guides beside it.
The panel switches member, scale, upscaler, quality, sharpness and guides live, and a split puts the raw image next to the reconstructed one.
At a scale below native the tracer traces smaller, with `reconstruct_jitter`'s offset, and the raw side shows the smaller image pixel for pixel.
Turning `denoise` off while upscaling shows what an upscaler alone does to path-tracing noise.

It opens on the temporal half — one sample a pixel, svgf, permanently noisy on the left of the split — because that is the half that shows what a denoiser is for.
Turning `fresh samples` off switches to the accumulating half, where `samples` climbs and a spatial member backs off as the mean converges.

## Testing

- The front's policy — what `automatic` picks, that a named member it cannot run writes nothing — runs on WARP through à-trous.
- à-trous itself: a flat image stays flat, a guide edge does not bleed, and a deep mean is left close to itself.
- OIDN: every operation against a reference implementation, tiled against whole on aligned, unaligned and one-axis shapes, and the network against Intel's own filter.
- SVGF itself: a static noisy stream converges, a moving one is followed through its motion vectors, and a depth jump or a reset drops the history rather than ghosting it.
  The moving test is the one that pins reprojection at all.
  It runs the same shifting image twice — once with an honest motion vector, once told nothing moved — and requires the honest one to converge substantially further.
  A stream of zero motion alone would pass with the sign flipped, the half-pixel offset missing, or the motion texture bound to the wrong slot.
- FSR: its ratios and jitter sequence, how the front resolves it, and two conventions pinned by comparison.
  The jitter's sign — a static scene reconstructs visibly worse with it flipped — and its motion: a drifting scene with honest motion beats the same scene told nothing moved.
  Neither is a threshold on one run, since FSR renders something plausible under either convention.
  The front's composition — denoiser, then upscaler, restarting when the denoiser changes — and its refusal without depth and motion are tested beside them.
- Every member, once it exists, gets the same property test: the error against a converged reference falls.
  A vendor member's version is gated on its hardware and reports "not run" elsewhere rather than passing.
  Reference images from vendor members are never committed, since they change with the driver.
