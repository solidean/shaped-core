# Denoising

One call denoises a path-traced image, whichever denoiser runs behind it.
`sr::denoise_routine` is the front: a caller names a method, or `automatic`, and the front forwards to the member routine that implements it.
Every member is a routine of its own too, callable directly with its full options.

This is the design, including the parts not built yet.
[denoise.hh](../src/shaped-rendering/denoise.hh) is the API, and [structure.md](structure.md#denoising-in-progress) says what exists.

## The members

| member | kind | where it runs | status |
|---|---|---|---|
| `atrous` | spatial | dx12, vulkan (HLSL through DXC); WARP included | done |
| `svgf` | temporal | dx12, vulkan (HLSL through DXC) | done |
| `oidn` | spatial, trained | dx12, vulkan (HLSL through DXC) | done; named only, not real-time |
| `dlss_rr` | temporal, upscales | NVIDIA RTX; dx12 | done, SDK fetched on request |
| `fsr_rr` | temporal, upscales | AMD RDNA 4; dx12 | planned |

**A spatial member reads one image; a temporal one also reads history reprojected by motion vectors.**
The temporal ones work from about one sample per pixel, but only if every pixel's motion is known.

**The vendor upscalers are not members.**
DLSS Super Resolution, FSR 3.1 and FSR 4 are temporal upscalers, and on path-tracing noise they smear it rather than remove it.
The vendor product that denoises is DLSS Ray Reconstruction, which upscales as part of it.

**`fsr_rr` is not the member this table first described, and the name is now the only part of "Ray Regeneration" that survives.**
FidelityFX SDK 2.3.0 ships two separate effects, and neither is a single-image denoising upscaler.
`ffx_upscale.h` takes colour, depth and motion and does not denoise.
`ffx_denoiser.h` is **split-signal**: separate dispatches for direct diffuse, direct specular, indirect diffuse, indirect specular, ambient occlusion and specular occlusion.
So the member is re-scoped to that denoiser: temporal, no upscaling, and requiring `split_diffuse_specular` and `hit_distance` on top of what `dlss_rr` needs.
That is the same guide contract NRD wants, which is the argument for doing the tracer work once and getting both.
It is not built, and what it waits for is the tracer rather than the SDK.

**The native members are what CI tests.**
à-trous and SVGF are our own HLSL, so they run on WARP, and the front's policy is tested through them.
NRD — vendor-neutral, real-time, compute shaders — stays on the roadmap, and what it waits for is the tracer rather than the denoiser.
It wants radiance split into diffuse and specular, with hit distances.

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
`denoise_settings::quality` picks between them the same way, `fast` running the small one; there is no large network for these guides.

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

**Reconstruction from day one, named for what it does today.**
The API admits an output larger than the input, so a vendor member can upscale without the front changing shape.

- Everything the tracer produces is at the **input** extent, in input pixels: the colour, every guide, motion vectors and jitter.
  Only `denoise_inputs::output` is at the output extent.
- A caller never computes a ratio.
  It picks a `render_scale_preset` and asks `sr::denoise_input_extent` what to trace.
  A spatial member answers every preset with the output's own extent, so a caller cannot ask for a ratio a member would reject.
  So does an upscaling member this device cannot run: the call is about to be refused, and a caller that traced smaller for it would composite a smaller image into its own output.
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
Each field in `sr::denoise_settings` says which members read it, and a member ignores the rest, so switching members keeps every knob that still means something.
A member's own options — the full vendor surface — live on the member, never in the shared struct.

**Whether a call carries fresh samples is one of those knobs, not an argument.**
`denoise_settings::fresh_samples` is what tells `automatic` to pick among the temporal members.
`sr::denoise_input_extent`, `sr::resolve_denoise_method` and `sr::denoise_routine::execute` all read that one answer.
It sits in the struct rather than beside each call because planning a frame and running it are three calls apart.
A caller that said yes to one and nothing to another would have traced for a member the call then does not use.

**A denoised image keeps the alpha it came in with.**
Every member copies `denoise_inputs::color`'s alpha into `output` and writes only rgb, so a caller compositing with alpha gets the same channel whichever member ran.
SVGF carries a per-pixel variance in alpha between its own passes and swaps it for the caller's on the last one.
Whether a vendor member can honour this is open — it may write its own alpha and leave us no say — and that is the point at which the rule is either kept by a copy pass or relaxed in writing.

## Selection and refusal

**Explicit means explicit.**
Naming a member this build or device cannot run reports `unsupported`, logs once per process on sr's domain, and writes nothing.
Only `automatic` chooses, walking the members best first:
`dlss_rr`, `fsr_rr`, `svgf`, then `atrous` for a caller feeding fresh frames; `atrous` alone for a caller denoising a converging mean.
`oidn` is never chosen: at roughly 0.2 s per megapixel it is a reference-quality member rather than a frame-loop one, so a caller names it.

A silent fallback would make a comparison between two named members compare one with itself, which is the failure the framework's three-state readiness exists to prevent.

**Members are acquired when the call runs, never through dependency tokens.**
A token holds its holder pending until its whole subtree is ready, so one member this device cannot initialize would hold every other one hostage.
Instead the front's `init` prewarms every supported member, so prewarming the front still starts their compiles on the next tick.

## History belongs to the caller

`sr::denoise_history` is the images a member keeps between calls for one image stream: a temporal member's history, and the scratch a spatial member ping-pongs through.
It is move-only, since a copy would fork a history, and the caller holds one per stream.

**It holds textures, plus one object of the member's own.**
State that is not a texture — OIDN's network today, a vendor member's *feature handle* later — sits in a type-erased `std::shared_ptr<void>`, so `denoise.hh` names no member's type.
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
- **The vendor SDKs are fetched on request, never by default.**
  DLSS and FSR sit in sr behind `SR_HAS_<VENDOR>` and link PRIVATE, like SDL3.
  DLSS is wired: `extern/dlss/fetch-dlss.py` is a deliberate act nothing in dev.py performs, because its license is NVIDIA's own rather than one a build accepts on anyone's behalf.
  Without it `SR_HAS_DLSS` is 0, the routine still exists, and `dlss_rr` reports `unsupported` — the shape `sr::window_system` takes without SDL3.
  Whether OIDN is fetched by default — its CPU build is the one non-native member CI could run — waits on measuring its size.
  Intel's OIDN library is on request too, for the oracle test alone; its 2.5 MB of weights, two networks, are a default fetch, since the member runs them.

## Seeing it

`uv run dev.py example shaped-rendering/denoise-playground` is the whole thing on screen.
A small analytic path tracer writes the noisy colour and the guides beside it, the panel switches member, quality, sharpness and guides live, and a split puts the raw image next to the denoised one.

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
- Every member, once it exists, gets the same property test: the error against a converged reference falls.
  A vendor member's version is gated on its hardware and reports "not run" elsewhere rather than passing.
  Reference images from vendor members are never committed, since they change with the driver.
