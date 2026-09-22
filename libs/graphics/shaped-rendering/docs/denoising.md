# Denoising

One call denoises a path-traced image, whichever denoiser runs behind it.
`sr::denoise_routine` is the front: a caller names a method, or `automatic`, and the front forwards to the member routine that implements it.
Every member is a routine of its own too, callable directly with its full options.

This is the design, including the parts not built yet.
[denoise.hh](../src/shaped-rendering/denoise.hh) is the API, and [structure.md](structure.md#denoising-in-progress) says what exists.

## The members

| member | kind | where it runs | status |
|---|---|---|---|
| `atrous` | spatial | every sg backend, WARP included | done |
| `svgf` | temporal | every sg backend | done |
| `oidn` | spatial | every sg backend | done, weights fetched on demand |
| `dlss_rr` | temporal, upscales | NVIDIA RTX; dx12, vulkan | planned |
| `fsr_rr` | temporal, upscales | AMD RDNA 4; dx12 | planned |

**A spatial member reads one image; a temporal one also reads history reprojected by motion vectors.**
The temporal ones work from about one sample per pixel, but only if every pixel's motion is known.

**The vendor upscalers are not members.**
DLSS Super Resolution, FSR 3.1 and FSR 4 are temporal upscalers, and on path-tracing noise they smear it rather than remove it.
The vendor products that denoise are Ray Reconstruction and Ray Regeneration, and both upscale as part of it.

**The native members are what CI tests.**
à-trous and SVGF are our own HLSL, so they run on WARP, and the front's policy is tested through them.
NRD — vendor-neutral, real-time, compute shaders — stays on the roadmap, and what it waits for is the tracer rather than the denoiser.
It wants radiance split into diffuse and specular, with hit distances.

**OIDN's network is ours to run, and it is run rather than called.**
Intel's own GPU kernels are CUDA, HIP, SYCL and Metal built on vendor GEMM libraries, and its CPU device would mean a download and an upload every frame.
The weights are a separate Apache-2.0 repository, and the network they describe is sixteen 3x3 convolutions with a bias and a ReLU, four 2x2 max pools and four nearest upsamples with a skip concat.
So shaped-rendering runs it in its own compute shaders, on every backend sg has, with no exportable memory and no round trip.

The topology is a table in `impl/oidn_network.cc` and the layer widths come out of the weights file.
That split is what keeps a weights bump honest: a changed layer count fails to find its tensor, and a changed width fails the shape test beside it.

**The network runs in tiles, and its memory is why.**
It holds twenty-five feature maps at once, because the skips have to stay live across the whole decoder.
Run whole that is 5.4 MiB at 64x64, 2.7 GiB at 1080p and 10.7 GiB at 4K, measured by `oidn_network::feature_bytes_for` rather than estimated.
So the tensors are sized by a tile instead, 384 pixels by default, which is about 197 MiB whatever the image is.
Half precision would halve the untiled figure and settle nothing.

**A convolution thread produces a RUN of texels, which is what made the network affordable at all.**
Producing one texel per thread spends a whole row of weights on a single output and reuses none of it.
Producing several spends the same row on all of them, and one loaded input row serves all three kernel columns instead of being fetched three times.
That one change took a 256x256 tile from 91 ms to 18.5.

**The weights are stored with the OUTPUT channel innermost, because that is the dimension a wave varies.**
A lane's output channel is what differs across the wave, so the original `[o][ky][kx][i]` made a single weight load touch sixty-four rows scattered `9 * in_channels` floats apart.
Turning it inside out to `[ky][kx][i][o]` makes that load one or two cache lines, and is worth about 1.25x once the blocking is re-tuned.

**Every channel count is padded to a multiple of four, so the source is read four channels at a time.**
This is OIDN's `tensorBlockC` in our own terms, and the measurement asked for it.
Hoisting the input reads out of the channel loop took a tile from 7.4 ms to 2.6, so they were two thirds of the time.
A channel is contiguous within a texel, so one `float4` fetches four of them.
Only three of the network's shapes are not already a multiple of four: the nine input channels, the three output ones, and the seventy-three `dec_conv1a` concatenates.
So the padding costs almost nothing to compute.
A padding channel carries a hard zero and a weight of zero, because a NaN left in memory would survive being multiplied by nothing.
Together with a re-swept run of eight texels that is 7.6 ms to 5.3.

**It is worth less than the instruction count suggests, and that says where the limit now is.**
Four times fewer load instructions bought 1.4x rather than 4x.
The eighteen texels of a window are far apart in memory, so a `float4` and a `float` from the same texel cost the same cache line.
So what remains is memory divergence rather than issue rate.
The fix for that is a layout where a window's texels are contiguous, which is CHW — the layout OIDN's CPU device uses and its GPU device does not.

**Blocking the output channels as well was tried under both weight layouts and does not pay.**
16x1 is 8.0 ms where 16x2 is 10.9 and 8x2 is 9.4.
The sixty-four lanes of a wave already read the same input, so that traffic is a broadcast rather than something a second blocking dimension could amortize.
The registers it costs therefore buy nothing back.
This is the one place where the obvious next step is measurably wrong, which is why it is written down rather than left to be retried.

**Against OIDN's own GPU device we are still far behind, and that is the comparison that matters.**
Their CUDA device filters a 256x256 tile in 0.67 ms against our 5.3, and a whole 1080p frame in 20.6 ms against our 504 — 8x and 24x.
Both were timed the same way: device-resident buffers, warmed, best of several, with only the filter and its sync inside the clock.
The CPU comparison flatters us and is not the bar — for the record it is 30 ms against our 5.3 at 256x256.

**The gap is architectural rather than a matter of tuning.**
OIDN's GPU path is `cutlass::conv::device::ImplicitGemmConvolution` over `TensorNHWC`, in fp16, on tensor cores.
The SM80 instantiation uses a `GemmShape<16, 8, 16>` instruction with a fused `LinearCombinationRelu` epilogue.
Their weights are `ohwi` and their activations `hwc`, with channels padded to eight; `CUDADevice::init` sets `tensorBlockC = 8` and says why, "required by Tensor Core operations".
Ours is fp32 SIMT, and a 1080p frame is about 1700 GFLOP the way we tile it, sustained at 3.4 TFLOP/s of roughly 20-25 peak.
So even a perfectly tuned fp32 kernel lands near 85 ms and is still 4x off: the rest is the matrix hardware, which on DirectX means cooperative vectors.
That is also what an SGL port cannot reach today, since SGL stays on the intersection of its backends and neither fp16 nor a matrix type is in it.

**Reproducing the GPU comparison takes a file we deliberately do not fetch.**
`fetch-oidn.py` keeps the CPU device module and drops the CUDA, HIP and SYCL ones.
So `OpenImageDenoise_device_cuda.dll` has to be taken out of the upstream archive and put beside the core before `oidn::DeviceType::CUDA` will create.

**The default tile is 384 because that is where the curve flattens, and it trades memory against wasted work.**
The overlap is a fixed 80 per side, so a 384 tile keeps a 224 interior and a 256 tile keeps only 96.
Measured end to end on a 1080p frame: 384 takes 504 ms for 197 MiB and 768 takes 351 ms for 788 MiB.
So the curve keeps paying, but in memory — 768 is a third faster for four times 384's tensors, and an earlier sweep found 1024 regressing outright.

**A tile is 80 pixels wider than what it keeps, on every side, and 80 is measured rather than chosen.**
At that overlap a tiled image agrees with the same image run whole BIT FOR BIT, so the number is where the network's receptive field ends.
At 64 the two are 1.4e-03 apart, and with no overlap at all 2.8e-01 — which is what a seam looks like.
Nothing improves above 80, so it is a threshold rather than a quality knob.
OIDN derives its own the same way, as `tileOverlap = round_up(receptiveField / 2, tileAlignment)`.
Our measured 80 implies a receptive field of about 160, which is the range their base model sits in.
That agreement was found after the fact and is worth more than deriving it would have been: the number came from the image rather than from their source, and then matched it.

Their minimum tile is larger than ours, at `max(4 * tileOverlap, 768)`.
A 768 tile would cut a 1080p frame's wasted work from 3.2x to 2.3x and cost about 780 MiB of fp32 tensors, so it is a memory decision rather than a correctness one.

**An edge tile is shifted inward rather than allowed to hang over the image.**
Hanging over means filling the overhang by repeating the border pixel, and that smear is an image the whole-frame run never sees.
It moves the result, and it moves it further the wider the overlap is.
So before this was fixed the error GREW with the overlap, which is the opposite of how a halo behaves and is what gave the bug away.

**Binding groups are built with the network, not with a dispatch.**
A tile changes push constants and nothing a group names, so all twenty-four internal groups are made once and reused by every tile and every frame.
Only the two that name the caller's own textures are per call, and they are still not per tile.
Built per dispatch instead, a 1080p frame wants 240 tiles x 26 groups, which overruns the transient descriptor region — that is how this was found rather than reasoned about.

**That it computes what Intel computes is measured, not assumed.**
`oidn_filter_reference` runs OIDN's own filter over the same input, and the test compares the two.
The difference is a mean of 1.0e-05 and a worst of 7.5e-05 across a 64x64 image, which is what sixteen layers of fp32 on the GPU against their CPU inference costs.
The TILED path is held to the same standard by a second oracle: nine tiles over a 384x384 image land a mean of 1.1e-05 and a worst of 1.4e-04 against Intel's whole-image filter.
So tiling costs nothing measurable in agreement, and the first oracle alone would not have shown that, because 64x64 fits one tile and never tiles at all.
Reading the weights in the wrong source layout moves that mean to 0.29, four orders of magnitude out, which is the margin the bound is set against.
It is the only test that can ask the question: every other one checks a piece against its own definition, and a self-consistent mistake passes all of them.

**The radiance handed over is de-modulated, which is what keeps a surface's texture from being filtered as noise.**
NRD's input contract asks that radiance carry no material information, and `NRD_MaterialFactors` is the helper it ships for the purpose.
So that is what the repack divides by and the resolve multiplies back.
The factors are written to scratch by the repack rather than recomputed by the resolve, because NRD requires both directions to use the same ones.
Storing them makes that structural, instead of two passes independently agreeing on a camera, a normal and a roughness.
That is why `albedo` and `specular_albedo` are REQUIRED guides for this member rather than optional ones.

It is partial by construction, because NRD floors both factors well above zero and calls the specular half a biased solution.
On a checkerboard albedo under one flat normal, the case where nothing but the albedo says there is an edge, the member keeps about nine tenths of the contrast.
Feeding radiance straight through keeps under one tenth of it.

**Two conventions run the other way round from ours, and both are carried in settings rather than in a repack.**
NRD reads a motion vector as `pixelUvPrev = pixelUv + mv`, so its units are UV and its direction is previous minus current, where ours is pixels and current minus previous.
`motionVectorScale` carries the reciprocal extent and the sign, so the guide itself is handed over untouched.
Its matrices, despite what `NRDSettings.h` says in prose, are built column by column from the `float[16]`, which is `tg`'s own convention, so they are copied rather than transposed.

## The contract

**Reconstruction from day one, named for what it does today.**
The API admits an output larger than the input, so a vendor member can upscale without the front changing shape.

- Everything the tracer produces is at the **input** extent, in input pixels: the colour, every guide, motion vectors and jitter.
  Only `denoise_inputs::output` is at the output extent.
- A caller never computes a ratio.
  It picks a `render_scale_preset` and asks `sr::denoise_input_extent` what to trace.
  A spatial member answers every preset with the output's own extent, so a caller cannot ask for a ratio a member would reject.
  A free ratio can join later as one more way to ask.
- A change of either extent restarts the history, like a resize.

**Guides are declared by the member.**
`sr::required_guides(m)` is what a member cannot run without, and a call missing one reports `unsupported`.
`sr::optional_guides(m)` is what it uses when present.
A tracer writes the union for the members it may hand off between, in a few fixed tiers rather than one permutation per combination.

**Settings are one flat struct of knobs named for what they do.**
Each field in `sr::denoise_settings` says which members read it, and a member ignores the rest, so switching members keeps every knob that still means something.
A member's own options — the full vendor surface — live on the member, never in the shared struct.

## Selection and refusal

**Explicit means explicit.**
Naming a member this build or device cannot run reports `unsupported`, logs once per process on sr's domain, and writes nothing.
Only `automatic` chooses, walking the members best first:
`dlss_rr`, `fsr_rr`, `svgf`, then the spatial ones for a caller feeding fresh frames; `oidn`, then `atrous` for a caller denoising a converging mean.

A silent fallback would make a comparison between two named members compare one with itself, which is the failure the framework's three-state readiness exists to prevent.

**Members are acquired when the call runs, never through dependency tokens.**
A token holds its holder pending until its whole subtree is ready, so one member this device cannot initialize would hold every other one hostage.
Instead the front's `init` prewarms every supported member, so prewarming the front still starts their compiles on the next tick.

## History belongs to the caller

`sr::denoise_history` is everything a member keeps between calls for one image stream: temporal history, vendor feature handles, and the scratch images a spatial member ping-pongs through.
It is move-only, since a copy would fork a history, and the caller holds one per stream.

A routine is a per-context singleton and cannot know which stream a call belongs to, or when a stream has gone.
The caller can, which is the same reason sv keeps its accumulators in its per-view store rather than in a routine.
`reset()` is a camera cut: the next call starts from nothing and reports `restarted`.

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
  The switch is a hard cut today; running both for a few frames and crossfading is the planned refinement.
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
  Opening it names the resources foreign code will touch and how, so sg emits their barriers.
  It then hands out the native list and resources.
  Closing it records the declared states and invalidates the list's cached bindings.
  Without it a vendor SDK would bypass sg's barrier tracking silently.
- **OIDN needs nothing sg does not have either, because the member runs the network rather than the library.**
  Intel's own GPU devices would need exportable memory and shared fences, which sg has not got; its CPU device would need only a download and an upload, and costs a frame of latency for them.
  Running the weights ourselves is what avoids both, and it is why this is the one trained member that needs neither a vendor SDK nor a particular vendor's hardware.
  The library is still fetched, for the oracle test alone.
- **The vendor SDKs are fetched on request, never by default.**
  DLSS and FSR sit in sr behind `SR_HAS_<VENDOR>` and link PRIVATE, like SDL3.
  OIDN is the exception, and its size is why the question was open.
  It is Apache-2.0, so nothing about it is a license a person accepts, and it is fetched on demand like SDL3.
  The measurement settled it: the Windows release is 83 MB unpacked, and `OpenImageDenoise_core.dll` alone is 50.6 MB of that because the trained weights live inside it.
  So no install plan makes this dependency small.
  The CPU-only subset `extern/oidn/fetch-oidn.py` keeps is 52.8 MB — the same order as SDL3's 49.6 MB, which was already a default fetch.

## Testing

- The front's policy — what `automatic` picks, that a named member it cannot run writes nothing — runs on WARP through à-trous.
- à-trous itself: a flat image stays flat, a guide edge does not bleed, and a deep mean is left close to itself.
- SVGF itself: a static noisy stream converges, and a depth jump or a reset drops the history rather than ghosting it.
- Every member, once it exists, gets the same property test: the error against a converged reference falls.
  A vendor member's version is gated on its hardware and reports "not run" elsewhere rather than passing.
  Reference images from vendor members are never committed, since they change with the driver.
