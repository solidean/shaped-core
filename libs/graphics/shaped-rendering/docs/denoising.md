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
`dlss_rr`, `fsr_rr`, `svgf`, then the spatial ones for a caller feeding fresh frames; `oidn`, then `atrous` for a caller denoising a converging mean.

A silent fallback would make a comparison between two named members compare one with itself, which is the failure the framework's three-state readiness exists to prevent.

**Members are acquired when the call runs, never through dependency tokens.**
A token holds its holder pending until its whole subtree is ready, so one member this device cannot initialize would hold every other one hostage.
Instead the front's `init` prewarms every supported member, so prewarming the front still starts their compiles on the next tick.

## History belongs to the caller

`sr::denoise_history` is the images a member keeps between calls for one image stream: a temporal member's history, and the scratch a spatial member ping-pongs through.
It is move-only, since a copy would fork a history, and the caller holds one per stream.

**It holds textures and nothing else, which is what the next member changes.**
A vendor member keeps a *feature handle* — an object the SDK creates once for a resolution and a set of options, and that every later call passes back — and that is not an `sg::texture_2d`.
The successor is one owning pointer to a member-defined state object in place of the fixed array.
Each member declares its own struct, `_prepare` allocates the one the resolved method wants, and a member reaches its own through a checked cast.
That is deliberately not built yet.
It buys nothing for two texture-only members, and the port does not get harder while there are only two.
The caller's own declaration does not change either way, so it lands with the member that needs it.

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
- **OIDN on a GPU needs exportable memory and shared fences in sg.**
  OIDN on the CPU needs neither: download, filter, upload.
- **The vendor SDKs are fetched on request, never by default.**
  DLSS and FSR sit in sr behind `SR_HAS_<VENDOR>` and link PRIVATE, like SDL3.
  Whether OIDN is fetched by default — its CPU build is the one non-native member CI could run — waits on measuring its size.

## Seeing it

`uv run dev.py example shaped-rendering/denoise-playground` is the whole thing on screen.
A small analytic path tracer writes the noisy colour and the guides beside it, the panel switches member, quality, sharpness and guides live, and a split puts the raw image next to the denoised one.

It opens on the temporal half — one sample a pixel, svgf, permanently noisy on the left of the split — because that is the half that shows what a denoiser is for.
Turning `fresh samples` off switches to the accumulating half, where `samples` climbs and a spatial member backs off as the mean converges.

## Testing

- The front's policy — what `automatic` picks, that a named member it cannot run writes nothing — runs on WARP through à-trous.
- à-trous itself: a flat image stays flat, a guide edge does not bleed, and a deep mean is left close to itself.
- SVGF itself: a static noisy stream converges, a moving one is followed through its motion vectors, and a depth jump or a reset drops the history rather than ghosting it.
  The moving test is the one that pins reprojection at all.
  It runs the same shifting image twice — once with an honest motion vector, once told nothing moved — and requires the honest one to converge at least twice as far.
  A stream of zero motion alone would pass with the sign flipped, the half-pixel offset missing, or the motion texture bound to the wrong slot.
- Every member, once it exists, gets the same property test: the error against a converged reference falls.
  A vendor member's version is gated on its hardware and reports "not run" elsewhere rather than passing.
  Reference images from vendor members are never committed, since they change with the driver.
