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
| `oidn` | spatial | every sg backend, WARP included | done, weights fetched on demand |
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
So the tensors are sized by a tile instead, 384 pixels by default, which is about 195 MiB whatever the image is.
Half precision would halve the untiled figure and settle nothing.

**A convolution thread produces a RUN of texels, which is what makes the network affordable.**
The weights dominate: every thread in a wave reads a different output channel's row, so those reads are strided where the input reads are a broadcast.
Producing one texel per thread spends a whole row of weights on a single output and reuses none of it.
Producing 32 spends the same row on 32 outputs, and one loaded input row serves all three kernel columns instead of being fetched three times.
Swept over a 256x256 tile: 1 texel is 91 ms, 8 is 18.5, 16 is 12.2, 32 is 11.6, and 48 falls back to 16.4 as the accumulators spill.

**Where that leaves us against Intel, measured rather than claimed.**
At 256x256 OIDN's own CPU filter takes 30 ms on this machine and ours now takes 11.6 ms, so per pixel we are about 2.6x faster than their CPU implementation.
A whole 1080p frame takes 1.04 s, against tens of seconds before — it hung a test watchdog rather than finishing.
Per pixel we are ahead; per FRAME we are roughly level with their CPU, because the overlap makes us compute 6.6 Mpx to deliver 2.1 Mpx.
So the next real win is the overlap rather than the shader.

**The default tile is 384 because that is where the curve flattens, and it trades memory against wasted work.**
The overlap is a fixed 80 per side, so a 384 tile keeps a 224 interior and a 256 tile keeps only 96.
Measured end to end on a 1080p frame: 256 takes 2.8 s for 87 MiB, 384 takes 0.98 s for 195 MiB, and 512 takes 0.94 s for 346 MiB.
So 512 buys almost nothing for nearly twice 384's memory.

**A tile is 80 pixels wider than what it keeps, on every side, and 80 is measured rather than chosen.**
At that overlap a tiled image agrees with the same image run whole BIT FOR BIT, so the number is where the network's receptive field ends.
At 64 the two are 1.4e-03 apart, and with no overlap at all 2.8e-01 — which is what a seam looks like.
Nothing improves above 80, so it is a threshold rather than a quality knob.

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
  Running the weights ourselves is what avoids both, and it is why the one trained member is also the one that works on WARP.
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
