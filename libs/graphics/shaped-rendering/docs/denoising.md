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
| `oidn` | spatial | CPU; NVIDIA, AMD, Intel and Apple GPUs | planned |
| `dlss_rr` | temporal, upscales | NVIDIA RTX; dx12, vulkan | planned |
| `fsr_rr` | temporal, split-signal | dx12 | planned — see below |
| `nrd` | temporal, split-signal | every sg backend, WARP included | done, sources fetched on request |

**A spatial member reads one image; a temporal one also reads history reprojected by motion vectors.**
The temporal ones work from about one sample per pixel, but only if every pixel's motion is known.

**The vendor upscalers are not members.**
DLSS Super Resolution, FSR 3.1 and FSR 4 are temporal upscalers, and on path-tracing noise they smear it rather than remove it.
The vendor products that denoise are Ray Reconstruction and Ray Regeneration, and both upscale as part of it.

**The native members are what CI tests, and `nrd` joins them.**
à-trous and SVGF are our own HLSL, so they run on WARP, and the front's policy is tested through them.

**NRD is a planner rather than a renderer, which is why it runs everywhere.**
It compiles nothing at run time, owns no device memory and records nothing.
What it answers is "which compute dispatches would denoise this frame, against which resources, with which constants", and sr executes that answer through sg.
So it needs no native scope and no vendor runtime, and it is the only split-signal member that can be tested without the hardware that shipped it.
That makes it the reference the `fsr_rr` member will be judged against, since the two want the same guides.

**Its encodings are exact formulas, not conventions, and we never reimplement them.**
NRD does not take a normal, a roughness and a hit distance as such.
It takes one normal-roughness texture in an encoding its own build chose, and radiance in YCoCg whose alpha carries a hit distance normalized against a curve of view depth and roughness.
So `nrd_repack.hlsl` and `nrd_resolve.hlsl` include NRD's own `NRD.hlsli`.
[extern/nrd/CMakeLists.txt](../../../../extern/nrd/CMakeLists.txt) copies it into sr's shader directory at configure time, because a shader package resolves every include under one source directory.
A reimplementation that drifted would be a worse image rather than a build error.
`nrd_session::create` refuses outright a library whose reported encodings are not the ones the repack target's format assumes.

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
`dlss_rr`, `fsr_rr`, `nrd`, `svgf`, then the spatial ones for a caller feeding fresh frames; `oidn`, then `atrous` for a caller denoising a converging mean.

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
  Opening it names the resources foreign code will touch and how, so sg emits their barriers.
  It then hands out the native list and resources.
  Closing it records the declared states and invalidates the list's cached bindings.
  Without it a vendor SDK would bypass sg's barrier tracking silently.
- **OIDN on a GPU needs exportable memory and shared fences in sg.**
  OIDN on the CPU needs neither: download, filter, upload.
- **The vendor SDKs are fetched on request, never by default.**
  DLSS and FSR sit in sr behind `SR_HAS_<VENDOR>` and link PRIVATE, like SDL3.
  Whether OIDN is fetched by default — its CPU build is the one non-native member CI could run — waits on measuring its size.

## Testing

- The front's policy — what `automatic` picks, that a named member it cannot run writes nothing — runs on WARP through à-trous.
- à-trous itself: a flat image stays flat, a guide edge does not bleed, and a deep mean is left close to itself.
- SVGF itself: a static noisy stream converges, and a depth jump or a reset drops the history rather than ghosting it.
- Every member, once it exists, gets the same property test: the error against a converged reference falls.
  A vendor member's version is gated on its hardware and reports "not run" elsewhere rather than passing.
  Reference images from vendor members are never committed, since they change with the driver.
