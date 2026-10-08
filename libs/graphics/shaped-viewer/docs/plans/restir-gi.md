# ReSTIR GI for the interactive mode

**A plan, being built: phase G0 has landed, and everything past it is proposed.**
It is the design of the traced lighting in sv's interactive mode, which the user chose to be ReSTIR GI rather than a radiance cache.
The decided direction it serves ([the roadmap](rendering-roadmap.md)):

- **Interactive**: rasterized primaries, lit by ray tracing, with indirect light resampled by ReSTIR GI.
- **Final**: the wavefront tracer converging, unbiased, as plain path tracing.
- **Later**: special handling for mirrors and caustics.

ReSTIR DI, for the direct light of scenes with many lights, runs in the same plan as a second track beside GI.

The document says which parts are **published** and which are **proposed**.
[What ReSTIR GI is](#what-restir-gi-is-published) describes the papers, cited in [References](#references); everything from [How it fits sv](#how-it-fits-sv) onward is a proposal.
[Decisions](#decisions) records what the user has settled.
Every cost below is either **measured**, on today's tracer, or an **estimate** for what is not built, and says which.

## Decisions

Settled by the user:

1. **When the camera stops, traced or hybrid, the image hands over to the converging wavefront through the existing crossfade.**
   Only after the drop to one plain sample at the stop is fixed; [The camera stops](#the-camera-stops) is that fix.
2. **ReSTIR is a per-layer setting on a traced layer's frames that feed the temporal denoiser; the hybrid render mode implies it.**
   So the GI phases 0 to 2 land and are tested on traced primaries, before raster primaries exist.
3. **A candidate's path runs to full depth, with roulette, by default**, so the moving image has the still mean's brightness.
   One-bounce GI is an explicit speed option, not the default.
4. **Final renders are plain path tracing, plus power-weighted light selection now.**
   Unbiased ReSTIR DI joins them later; ReSTIR PT is not planned.
5. **ReSTIR DI runs in parallel with GI phase 1**, after power-weighted light selection, rather than after the hybrid.
   The two tracks build their shared pieces once ([Phasing](#phasing)).

Proposed, from the independent review, and awaiting the user's confirmation:

6. **Reverse visibility is skipped by default**, with an `exact` switch that traces it, which the tests use.
   The acceptance test stays strict: normals within 25° and a plane distance relative to depth.
7. **The final shading re-evaluates the material per permutation**, from a persisted primary hit.
   It is measured against a reduced stored closure, with the extra dispatches counted in its cost.
8. **A scene edit resets the reservoirs in the first slice**; revalidation waits for scene items with an identity that survives a frame.
9. **The interactive budget is decided from per-round costs**: [Why](#why) now has the live-path counts, which put a moving frame's work in its first four rounds.
   The cost per round is inferred from those counts, not timed.
   1080p will likely need half-rate candidates.
10. **A device without ray tracing gets the raster path's direct lighting**, unless sg brings the ray-query polyfill to dx12 and vulkan.
    The roadmap assumes that polyfill, and it exists on webgpu alone, where the wavefront is out of scope; sg's TODO now carries it.
11. **Patents go to counsel.** NVIDIA holds patents in this area, among them US 11,922,556 B2; whether any covers this plan is not a question this document answers.

## Why

Measured on Intel's Sponza in daylight, in a 1280x720 window, on a laptop RTX 5070 with the GPU otherwise idle, October 2026:

| frame | GPU time per stage, summed over the frame's rounds | total |
|---|---|---|
| moving: 640x360 traced, 22 drain rounds | begin 0.5, extend 3.6, segment 1.7, sort 0.7, shade 9.8, connect 4.0, finalize 0.8, args 0.6, resolve 0.2 | ~22 ms |
| still: 1280x720, one round | begin 1.5–2.0, extend 4.0–4.6, segment 1.4–1.6, sort 0.8–1.5, shade 10.5–15.2, connect 4.9–7.0, finalize 1.2–1.8, resolve 0.7–1.3 | ~28 ms |

- **The moving frame's wall time is ~24.7 ms**, with ~8 ms of CPU recording, 2.5 ms of it in `trace_layer`.
- **Every slot was live in a still round at full fill**, since finalize regenerated it, so the round ran at full occupancy: ~30 ns per path segment.
- **A still frame's 16 ms budget fits no round at full occupancy.**
  A still frame now runs one round and starts samples in only a share of the free slots, as [wavefront.md](../wavefront.md#a-still-frame-fits-its-budget-by-rounds-then-by-fill) describes.
- **à-trous denoising of a still frame takes ~6.3 ms.**
- **Texture bandwidth is not the bottleneck**: sampling mip 4 at every hit saved at most 2 ms of shade.
- **A moving frame's live paths per round**, through `wavefront_trace_desc::live_paths` ([wavefront.md](../wavefront.md#what-it-measured)), stable to ~1% across frames (round 4 to ~3%):

  | round | 0 | 1 | 2 | 3 | 4 | 5–21 |
  |---|---|---|---|---|---|---|
  | live paths | 230,400 | ~208,000 | ~185,000 | ~164,000 | ~7,400 | 0 |

  So a moving frame traces ~795,000 segments in ~22 ms, ~28 ns each, close to the still round's ~30 ns at full occupancy.
  That figure includes the frame's fixed costs.
  Extend through connect alone, ~19–20 ms, gives ~24–25 ns per segment.
  Rounds 0–3 hold ~99% of the segments.
  The drain's 17 empty rounds still run every dispatch, and only args' share of their cost is known.
  Args does the same work in every round, ~27 µs, so ~0.5 ms over the 17.
  Round 4 is short because roulette runs once, in round 3's shade, and with `max_bounces = 5` round 4's shade ends every path it spares.
  That puts survival near roulette's 0.05 floor, if ~90% of round 3's ~164,000 paths hit as in rounds 0–2, since 0.05 of those is ~7,400.
  The hit rate in round 3 is inferred, not measured.
  So where a moving frame's work ends is set by `max_bounces` and `wavefront_roulette_after`, not by the scene.
  A higher cap leaves a longer, sparse tail.

A moving frame is one path per pixel, and the temporal denoiser receives that path's noise.
ReSTIR GI lets each pixel reuse the paths its neighbors and its own past found, and Ouyang et al. measure 9.3x to 166x lower mean squared error than path tracing at one sample per pixel.
**It costs more per frame, not less**: [Cost](#expected-cost-and-quality) estimates ~6–8 ms on top of today's moving frame.

## What ReSTIR GI is (published)

### Resampled importance sampling

RIS (Talbot et al. 2005) draws `M` candidates `x_i` from a source density `p(x)` that is easy to sample, and keeps one, `y`, with probability proportional to `w_i = p̂(x_i) / p(x_i)`.
`p̂` is the **target function**: any non-negative function shaped like the integrand, which need not be normalized or exact.
The kept sample comes with an **unbiased contribution weight** `W = (1 / p̂(y)) · (1 / M) · Σ w_i`, which stands in for `1 / pdf(y)`.
An integral `∫ f` is then estimated by `f(y) · W`.

A **reservoir** makes this streamable: it holds the kept sample, the running sum of weights and the count `M`, and two reservoirs merge in constant time.
In ReSTIR's original form (Bitterli et al. 2020), merging a reservoir resamples its kept sample with weight `p̂(y) · W · M`.
The passes proposed here use GRIS's form instead, below, where the `1 / M` lives in the MIS weights.

ReSTIR (Bitterli et al. 2020) applies this to direct lighting: candidate light samples per pixel, then reuse from the pixel's previous frame (temporal) and from nearby pixels (spatial).

### The sample and the reservoir

ReSTIR GI (Ouyang et al. 2021) resamples **paths**, cut at their second vertex.
A sample is generated at a pixel's **visible point** `x_v` (the primary hit) and stores:

- `x_v` and its normal `n_v`, where the sample was generated;
- the **sample point** `x_s` and its normal `n_s`: the surface a ray from `x_v` hit;
- `L̂_o`: the radiance leaving `x_s` toward `x_v`, estimated by tracing on from `x_s`;
- the path's random numbers.

A reservoir holds one such sample, its weight `W` and its count `M`.
The paper keeps three per-pixel buffers: this frame's initial samples, the temporal reservoirs, and the spatial ones.

### Initial sampling

At each visible point a direction is sampled from a known source density, a ray along it finds `x_s`, and path tracing from `x_s` estimates the radiance it sends back.
That candidate becomes a reservoir with `M = 1`.

### The target function

The paper states two (its Equations 9 and 10):

- `p̂ = L̂_o · f · cos θ`, the full contribution at `x_v`;
- `p̂ = L̂_o` alone, which the paper uses for temporal resampling, and recommends for spatial reuse because a sample then means the same to every pixel.

### Temporal reuse

The pixel's reservoir from the previous frame is found by reprojecting with motion vectors, then merged with the new candidate.
`M` is capped at 30 for temporal reservoirs, so old samples cannot outweigh new ones forever.

### Spatial reuse and the reconnection shift

A pixel merges reservoirs of neighbors within a screen-space radius; the spatial reservoir's `M` is capped at 500.
A neighbor's sample was generated at **its** visible point, so reusing it here **reconnects**: `x_s` and its radiance are kept, and connected to this pixel's `x_v`.
Both densities are in solid angle at different points, so the weight carries the Jacobian of that change of variables (the paper's Equation 11).
For a sample moved from pixel `q` to pixel `r`, with `x_1` the visible point, `x_2` the sample point, and `φ` the angle at `x_2^q` between its normal and the direction to each visible point:

```
|J(q → r)| = (|cos φ_2^r| / |cos φ_2^q|) · (‖x_1^q − x_2^q‖² / ‖x_1^r − x_2^q‖²)
```

The paper divides the target function by `|J|`.

**Visibility is where its variants part.**
The unbiased variant traces a ray from `x_v^r` to `x_s` to validate the reconnection (its Algorithm 4).
The default, biased variant traces nothing and accepts a neighbor by a geometric test: normals within 25° and normalized depths within 0.05.

### Final shading

The pixel's indirect light is `f(x_v, ω_y) · |cos θ_y| · L̂_o(y) · W`, with `ω_y` the direction from `x_v` to the kept `x_s`.
The direct light at `x_v` is computed apart, by next-event estimation or by ReSTIR DI.

### Bias

**ReSTIR GI is biased by default; an unbiased variant with visibility rays is presented.**
Two things bias the default:

- **Reuse without visibility**: a neighbor's sample reconnected through an occluder still counts, and the normalization counts samples this pixel could never have generated.
- **Direction-independent `L̂_o`**: reconnecting keeps the radiance `x_s` sent toward the original visible point, which is right only for a Lambertian `x_s`.
  The visibility ray does not remove this one.

### Generalized RIS

GRIS (Lin et al. 2022) is the theory that makes the resampling itself unbiased.
It takes candidates from different domains, correlated and with unknown densities, and maps each into the pixel's domain by a **shift mapping** `T_i`:

- a candidate contributes `w_i = m_i(T_i(X_i)) · p̂(T_i(X_i)) · W_i · |∂T_i / ∂X_i|` (its Equations 7 and 19);
- the kept sample's weight is `W_Y = (1 / p̂(Y)) · Σ_j w_j`, with no `1 / M` (Equation 22): the count lives in `m_i`, and `m_i = 1 / M` recovers the 2020 form.

It is **unbiased** when (Equation 20):

- the candidates' supports together cover the integrand's;
- every `m_i ≥ 0`, they sum to one over the domains that can produce `y`, and `m_i > 0` wherever a domain is needed;
- every `W_i` is itself an unbiased contribution weight.

A canonical sample — the pixel's own candidate, under the identity shift — is the simple way to guarantee the first.

The **generalized balance heuristic** gives such weights: `m_i(y) = p̂_{←i}(y) / Σ_j p̂_{←j}(y)`.
`p̂_{←j}(y)` is the target function of domain `j` at `y` shifted there, Jacobian included.
**Confidence weights** multiply each term by a reservoir's `M` (Section 6), and any positive values keep it unbiased, so capping `M` is no source of bias.
Its cost is quadratic in the candidates; **pairwise MIS**, generalizing Bitterli's thesis, comes close at a linear cost by weighing each neighbor against the canonical sample alone.

GRIS names three shifts for paths:

- **Reconnection**: keep the base path's vertices from some point on, and connect the new prefix to them — ReSTIR GI's shift.
  Its criteria (Section 7.5): the two vertices of the connection at least `d_max` apart, 1–5% of the scene's size, and both rough, by a per-lobe GGX roughness threshold of 0.2.
- **Random replay**: re-trace the base path's random numbers from the new pixel; it works through mirrors and glass, and costs a path per shift.
- **Hybrid**: random replay until the first vertex that meets the reconnection criteria, then reconnect there.

**ReSTIR PT**, in the same paper, resamples whole paths with the hybrid shift, unbiased, including glossy and specular transport.
Its real-time setup uses 20 candidates and 3 spatial neighbors within 20 pixels.
ReSTIR PT Enhanced (Lin et al. 2026) reports it 2–3x faster, with reciprocal neighbor selection, footprint-based reconnection, duplication maps, and DI and GI unified in shared reservoirs.

### What else was published

- **Production practice** (Wyman and Panteleev 2021): light presampling into tiles, a fused spatiotemporal pass, and what RTXDI ships.
- **World-space reservoirs** (Boissé 2021, AMD): reservoirs in a hash grid, so reuse reaches offscreen and disoccluded surfaces.
- **Conditional RIS** (Kettunen et al. 2023): resampling conditioned on a path prefix, which puts ReSTIR on final gathers from a secondary vertex.
- **Decorrelation by MCMC mutations** (Sawhney et al. 2024): against the blotchy, correlated noise heavy reuse leaves.
- **The course** "A Gentle Introduction to ReSTIR" (Wyman et al. 2023) is the clearest single derivation, and the reference for implementing it.
- **RTXDI**, NVIDIA's SDK with ReSTIR DI, ReGIR and ReSTIR GI, is under the proprietary NVIDIA RTX SDKs license, as NRD is.
  Its documentation says its ReSTIR GI "converts all secondary surfaces into Lambertian reflectors or emitters", and recommends clamping their roughness.
- **NRD**'s readme says its RELAX denoiser "has been designed for RTXDI".

Implementations, by license:

| implementation | license | what it is |
|---|---|---|
| Bevy's Solari | MIT or Apache-2.0 | ReSTIR DI and GI with a world-space cache |
| Falcor | BSD-3-Clause | NVIDIA's research framework, home of the published ReSTIR work |
| DQLin/ReSTIR_PT | BSD-3-Clause | GRIS's reference implementation |
| kajiya | MIT or Apache-2.0, archived | ReSTIR GI: hemispherical rays, temporal `M` clamped at 10, two spatial passes |
| AMD Capsaicin | MIT | GI-1.x is radiance caching with screen probes and a hash grid, not ReSTIR GI |
| NVLabs/conditional-restir-prototype | NVIDIA Source Code License-NC | non-commercial: not usable here |
| RTXDI, NRD | NVIDIA RTX SDKs license | proprietary |
| Unreal Engine | Epic's EULA | not permissive |

Bevy's Solari, the most readable of them, takes one spatial neighbor within 30 pixels, tests normals within 25° and a plane distance of 0.3%, and rejects a Jacobian above 2.
On an RTX 3080 at 1600x900, upscaled to 3200x1800 by DLSS-RR, its two ReSTIR GI passes take 0.37 and 0.44 ms, ~0.81 ms together.

## How it fits sv

Everything from here on is proposed.

### Where it runs

**ReSTIR replaces the signal handed to the temporal denoiser, and nothing else** (decision 2).
A frame that sees the image change starts one sample per pixel, drains it, and writes `frame_samples` for the temporal member.
[reconstruction.md](../../../shaped-rendering/docs/reconstruction.md#meeting-a-progressive-path-tracer) describes that hand-off.
ReSTIR turns that one sample into a resampled estimate before it is written.

**The converging mean stays plain path tracing.**
Every still frame still adds its finished paths to the mean, unbiased, and no reservoir reaches it.
The young still frames that feed the temporal member also get a resampled `frame_samples` ([The camera stops](#the-camera-stops)), so the setting reads those as well.

**No temporal denoiser, no ReSTIR.**
Only a layer whose denoiser may run temporally writes `frame_samples`, and only such a layer traces scaled moving frames, so the setting has nothing to replace elsewhere.

The hybrid mode is the same pipeline with its primaries rasterized ([Raster primaries](#raster-primaries)).

### The initial sample is the path the wavefront already traces

**No new tracer: the wavefront produces the candidates.**
A moving frame's path visits `x_v` in round 0 and `x_s` in round 1, and drains the rest.
What changes is what the path remembers:

- **Shade at bounce 0 rebases the throughput.**
  It stores the continuation's factor `T_1 = f · |cos| / pdf` and its solid-angle pdf beside the path, and continues with throughput 1.
  Everything the path gathers from then on is `L_i(x_v, ω)`, the radiance arriving along the sampled direction.
  With reuse off, finalize adds `T_1 · L_i` to the direct light, which is today's estimator with a different roulette.
- **The direct light at `x_v` keeps a sum of its own.**
  The next-event sample at bounce 0, and the analytic light or environment the bounce-0 ray reaches with its MIS weight, go there.
  So `L_i` is exactly the light reflected at `x_s` and beyond; surface emission adds nothing at `x_s`, since sv counts it for camera rays only.
- **Shade at bounce 1 writes the candidate's geometry**: `x_s`, its geometric normal, and whether `x_s` is reconnectable.
- **Finalize completes the candidate** with `L_i` when the path ends.

**A candidate is invalid**, and its pixel falls back to its own path's estimate for the frame, when:

- **the segment from `x_v` to `x_s` scatters in a medium or crosses a transmitting interface.**
  A scatter does not count as a bounce (`segment_settle`), so the check is a flag the path sets, not the bounce count.
- **the path is unfinished when the frame resolves.**
  A drain is bounded at `max_bounces + 17` rounds and drops longer medium walks ([wavefront.md](../wavefront.md#where-it-stands)), which already biases today's moving image there.
- **the ray from `x_v` escapes or ends on a light.**

An invalid candidate counts in `M` as an empty sample, so the integrand splits cleanly.
Transport through a medium or a transmission on the first segment stays plain path tracing; everything else is resampled.
A pixel whose own `x_v` is inside a medium, or transmits, takes no shifted sample either.

The candidate's density is the BSDF sampler's full mixture pdf, which `openpbr.bsdf_sample::pdf` already carries for MIS.

**A candidate's path runs to full depth by default** (decision 3), as today's moving path does, so the moving image's mean is the still mean's.
**One-bounce GI is the speed option**: the path stops after the next-event estimate at `x_s`.
It drops the light of every later bounce, so its image is darker, and it is never the default.

### The primary hit must outlive its round

**`restir_shade` cannot read round 0's hit or sort ranges: every round overwrites them.**

- extend overwrites `traced.hits[s]`, and shade rewrites `path_rays[s]`;
- `wavefront_args_round` zeroes the per-permutation counters, and `wavefront_args_shade` rewrites `shade_offsets`;
- the sort overwrites `queue_sorted`;
- the generated kernel reaches its hit only through these: `tracer.shade_slot`, then `tracer.stored_triangle_context` reading `traced.hits`.

So the final shading needs pieces of its own:

- **A persisted primary hit per pixel**, `traced.restir_visible`: shade at bounce 0 copies the `hit_record` and the camera ray into it.
  G0 keeps it on draining frames only; a still frame of the camera-stop fix will write it whenever a path of the pixel passes bounce 0.
- **A re-shade round after the drain**, built in G0: the counters are cleared, `wavefront_restir_gather` queues each pixel by its permutation, and the round's own sort runs.
  Then each permutation's shade kernel runs again in `wavefront_step::mode`'s re-shade mode.
  `tracer.stored_hit` hands the kernel the persisted hit in that mode, and `shade_stored` branches to `restir_shade_stored`.
  That builds the closure exactly as the bounce-0 shade did, decals and shading frame included, and evaluates the BSDF toward the kept sample.
  The gather is its own kernel, so `counter_hits` still has one append site per kernel, as the NVIDIA driver bug needs.

**The plan had a second generated entry per permutation, `sv_restir_shade`; the mode replaced it.**
It costs no second pipeline per permutation, and one uniform branch in every shade kernel.
Each kernel is larger and compiles longer for it, since the re-shade's closure and decals are inlined beside the round's.
Whether the larger kernel lowers shade's occupancy is measured on Sponza before phase G1 lands, through the wavefront benchmark given a ReSTIR variant.
The viewer times still frames alone, and ReSTIR runs on restarting ones, so `sv.wavefront.restir` reaches a recording only from a caller that times one.
Before G1 the re-shade's identity is also checked on a glossy and a coated item: the box is all matte, and G1's target function rests on that agreement.
A path the frame splits carries `path_flag_split`, so a frame that finishes it later, a still one, recombines its light all the same.

G0's records cost ~170 bytes per traced pixel, ~39 MB at 640x360 and ~350 MB at 1080p, allocated on first use.

What it costs:

- **One more indirect dispatch per permutation per frame, ~28 on Sponza**, each with the bindless declarations its execute makes.
  Their CPU recording time is not measured; a moving frame records for ~8 ms today.

### The material at the visible point

**The target function may be approximate; the final shading may not.**
GRIS stays unbiased for any target function that is positive wherever the integrand is.
So `p̂` reads the visible surface record — the denoiser's own guides — and a GGX lobe built from them:

```
p̂(ω) = luminance((ρ_d / π + ρ_s · D_GGX(ω; roughness)) · |cos θ| · L_o)
```

Every resampling pass then evaluates it in one generic kernel, with no material code.
Including the BSDF follows the paper's Equation 9 rather than its Equation 10, since reuse here weighs each sample against the pixel's own lobe.

The final shading needs the exact BSDF (decision 7):

- **Re-evaluate the material** in `sv_restir_shade`, from the persisted hit; the starting point, exact for every lobe, at the cost above.
- **Store a closure** at bounce 0, and shade in one generic kernel, with no second entry and no sort.
  Only what `openpbr.bsdf_eval_split` reads, never the whole `openpbr.bsdf`: the media, the subsurface interior and the emission stay out.
  That is about 50 floats, ~100 bytes as halves: ~23 MB at 640x360 and ~92 MB at 1280x720.
- **Store a reduced closure** — diffuse, one specular GGX lobe and a coat — at 32–48 bytes, which is an approximation and biases the shading.
  It is the measurable baseline of phase 0.5, not a candidate for the default.

### The reservoir and where it lives

Per-pixel records, structure of arrays as the wavefront's state is:

| record | holds | bytes | copies |
|---|---|---|---|
| visible surface | `x_v`; shading and geometric normal, octahedral; diffuse albedo; specular albedo and roughness, as halves; flags | 40 | this frame and the last |
| GI reservoir | `x_s`; `n_s`, octahedral; `L_o` as halves; `W`; `M` and an age, as halves; flags | 36 | the history and this frame's |
| candidate | `x_s`, `n_s`, `L_i`, the source pdf, flags | 36 | one |
| persisted primary hit | the `hit_record` and the camera ray's direction | 48 | one |
| direct sum | a `float3`, padded | 16 | one |
| sorted primary list | a slot index | 4 | one |

That is ~256 bytes per traced pixel; the DI reservoir adds ~40 more.

| traced extent | for | memory |
|---|---|---|
| 640x360 | 720p, upscaled 2x | ~59 MB |
| 960x540 | 1080p, upscaled 2x | ~133 MB |
| 1280x720 | 720p native | ~236 MB |
| 1920x1080 | 1080p native | ~531 MB |

For comparison, the wavefront's own path state is about 500 MB at 1080p.

**The records live on the accumulation slot, beside the path state, one set per extent.**
`restir` sits beside `paths` and `scaled_restir` beside `scaled_paths`, in `impl::temporal_slot`, and the store's budget counts their bytes where it counts the paths'.
A 720p view holding both carries ~295 MB; the full-size set is needed only while the young still frames feed the temporal member, and may be dropped after the fade.

**They reset on what the temporal member's history resets on**: the scene hash, kept on the `frame_samples` slot as `reset_hash`, and a camera cut.
They do not reset on camera motion, which is what they exist to survive, nor on a change of extent ([The camera stops](#the-camera-stops)).

**The visible surface keeps its previous frame**, so the temporal pass can weigh a candidate against the surface it is reused from.
Storing `x_v` in the reservoir instead, as the paper does, saves the second copy and loses the previous closure, which the MIS weights need.

### Passes

After the drain and before resolve, each a dense dispatch over the traced pixels:

1. **`restir_temporal`**: the pixel's candidate, merged with its reprojected history.
   The history's sample is shifted to this frame's `x_v` by reconnection, with its Jacobian and one visibility ray.
   The two are weighted by the generalized balance heuristic, with `M` capped at about 20 as the confidence weight.
2. **`restir_spatial`**: up to `k` neighbors, merged by pairwise MIS with this pixel's temporal result as the canonical sample.
   Each costs a reconnection shift and a visibility ray.
   `k` is 1 or 2 by default, and more where the history is short, as after a disocclusion.
3. **The sort of the persisted primary hits**, then **`sv_restir_shade`** per permutation.
   It writes `frame_samples` and, for a split-signal member, `frame_diffuse`, `frame_specular` and the hit distance.

Whether the spatial output or the temporal one feeds the next frame is a tuning choice.
Feeding the spatial one back spreads samples faster and correlates them more.

**The resampling passes need no atomic.**
Each reads other pixels' reservoirs from the previous pass's buffer and writes only its own pixel, so every value has one writer.
The visibility rays are traced inline, inside the pass, rather than appended to a queue.

**Every pass draws its own random numbers**: `pt.hash` of the pixel, the frame's seed and a pass index.
`fc.rng_seed` alone is one value per scaled frame, so two passes seeded from it alone would draw one sequence.

### Raster primaries

**The hybrid seeds round 0 from a visibility buffer instead of tracing it.**
The raster pass writes instance and primitive per pixel, which `raster.varyings` already carries ([raster.md](../raster.md#the-g-buffer-seam)).
A small kernel intersects each pixel's camera ray with its triangle, and writes the `hit_record` extend would have: instance, primitive, barycentrics and `t`.
The wavefront then runs segment, sort and shade on round 0 as if it had traced it.
The raster pass needs no material: it writes depth and ids, and the material is evaluated once, in the sorted shade.

**The raster path is not ready to seed it; phase 3 has to add:**

- **The tracer's coverage.** Raster cuts coverage at one half, where the tracer's any-hit keeps a stochastic coverage for a cutoff of 0.
  The visibility pass reads the instance's opacity record and cuts stochastically, per pixel and frame, as the tracer does, or the hybrid's image differs from the traced one.
- **Jitter.** Raster shades every pixel at its center; the visibility pass has to take `sr::reconstruct_jitter`'s offset, which antialiases it and is what the upscaler needs.
- **A scaled extent.** Raster never draws below the view's size, and the hybrid's moving frames are traced at the upscaler's input extent.
- **What raster cannot draw yet**: quadric batches and layered transparency, which the hybrid inherits as open.

Traced primaries stay as the reference the hybrid is tested against.

### Reprojection, disocclusion, cuts and scene changes

**Reprojection uses the reservoirs' own camera and extent rather than the motion guide.**
`x_v` is projected into the camera and extent the history was written under, which the records keep.
For a static scene that is what `motion_guide` encodes, and it stays right across an extent change and a skipped frame.
Object motion, once scene items have an identity that survives a frame, needs the motion guide instead.

**A history is accepted when its surface is this one** (proposal 6), and otherwise the pixel starts from its candidate:

- the previous `x_v` lies within a small distance of this `x_v`'s tangent plane, relative to view depth;
- the shading normals are within 25°.

That rule applies the same way in both directions of the MIS weights, or it biases the result.

**A disocclusion** is a pixel whose history was rejected: it has `M = 1`, and spatial reuse with more neighbors fills it in.
The denoiser still sees it as young, through the per-pixel sample counts it already reads.

**A camera cut** resets the reservoirs where `camera_cut_pending` resets the denoiser's history today.

**A scene change resets them too, which costs an editor** (proposal 8).
Every frame of a dragged light changes the scene hash, so the reservoirs hold no history while something is edited.
Keeping the samples and re-estimating their `L_o` — revalidation, a round from `x_s` — is the published answer.
It needs to know which samples a change touched, which needs the per-item identity sv does not have yet.

### The camera stops

**Today the image drops to one plain sample at the moment the user stops** — the roadmap's second item.
A moving frame is traced at the scaled extent and its temporal member keeps `scaled_denoise`.
The first still frame is traced at full size, its mean restarts, and the full-size history resets because the last frame was scaled.
So the temporal member starts from nothing on one sample per pixel, exactly when the user looks closely.

**Scaled reservoirs alone would not fix it**: they are at the other extent, and the full-size stream has none.

The fix, which decision 1 waits for:

1. **ReSTIR runs on the full-size still frames that feed the temporal member**: the first `temporal_denoise_frames`, and the fade after them.
   The mean stays plain; only `frame_samples` is resampled.
   A still frame's paths carry across frames, so a pixel's candidate is the latest path of it that finished; one with none merges no new candidate that frame.
2. **The scaled reservoirs are the full-size ones' first history.**
   A reservoir is a world-space sample, `x_s` with its radiance and weight, and its records keep their camera and extent.
   So a full-size pixel reprojects into the scaled records and shifts their sample by reconnection, as from any other domain, and the first still frame starts with the moving stream's `M`.
   Four full-size pixels then share one scaled history, which caps their confidence weights rather than biasing them.
3. **The full-size denoiser history still starts from nothing.**
   Its first input is now as good as a moving frame's, so the step is the temporal accumulation it has not done yet, not a fall to one sample.
4. **The crossfade then hands over to the spatial member on the mean**, as today.

**Removing the remaining step is a choice for after measuring**, between two ways:

- **Seed the full-size history from the last upscaled output.**
  That is an sr extension, `sr::reconstruct_history` seeded from an image, which no member offers today.
- **Keep the scaled stream running through the hand-off**, and fade from its upscaled output to the mean.
  It costs a scaled frame on each young still frame, ~28–30 ms (estimated).

**What the fix costs**, estimated: the resampling and the re-shade at full size are about four times the scaled ones, ~24–32 ms on each young still frame on top of its round.
That is a lot beside a 28 ms round, and it is what phase 1 measures before the fix is taken as built.

### Upscaling

The reservoirs live at the traced extent, and the upscaler runs after the denoiser as today.
Inside one extent nothing about ReSTIR crosses extents: the records and the motion are in input pixels.
FSR's jitter moves `x_v` by a fraction of a pixel each frame, which the world-space acceptance test absorbs.

### The denoiser

**The resampled signal is far less noisy and differently noisy.**
ReSTIR's noise is correlated across neighbors and across frames, which a filter estimating per-pixel variance can mistake for detail.

- **svgf** runs on it unchanged, and may filter it too little; phase 1 measures that.
- **nrd**: NRD's readme says RELAX was designed for RTXDI, and sr's member runs REBLUR today; which one suits this signal is measured, not assumed.
- **dlss_rr** reaches the moving stream once its TODO item lands.
- **atrous** on the mean is untouched, since the mean is plain path tracing.

**Demodulation can be exact.**
The final shading evaluates the diffuse and specular lobes apart, so it can write the diffuse half divided by its albedo directly.
The members demodulating by the guides today divide noisy radiance by the albedo; this would divide nothing.
Handing over demodulated radiance is a contract change in sr, kept for after phase 2.

**The hit distance is half free.**
`|x_s − x_v|` of the kept sample is the diffuse half's hit distance.
The specular half keeps the pixel's own path's, as today, since the reservoir's sample says nothing about the specular lobe.

### Materials: what reuses and what does not

**Reconnectable means rough on both sides**, by GRIS's criterion: the minimum roughness of the lobes that carry significant weight, above about 0.2.
A minimum, because a clearcoat over a diffuse base is glossy however rough the base is.

| at `x_v` | what happens |
|---|---|
| diffuse, rough specular | reused; the case ReSTIR GI is for |
| glossy, below the threshold | not reused: a neighbor's direction falls outside a narrow lobe; the pixel keeps its own candidate |
| mirror, smooth metal, smooth glass | not reused: the pixel keeps its own path, which is plain path tracing |
| transmission, or inside a medium | not reused ([the candidate](#the-initial-sample-is-the-path-the-wavefront-already-traces)) |

| at `x_s` | what happens |
|---|---|
| rough, near Lambertian | reconnectable, nearly exact |
| rough but not Lambertian | reconnectable, and **biased**: `L_o` is view-dependent through its GGX lobes and its next-event estimate |
| below the threshold | the shift fails, and the MIS weights account for it; the sample is used only at its own pixel |

**The middle row is the Lambertian simplification RTXDI documents**, bounded here by the roughness threshold rather than removed.
Removing it means re-evaluating `x_s`'s BSDF on every shift with its stored incoming light, which is the step toward ReSTIR PT that decision 4 does not take.

**Mirrors and caustics stay the later item they are.**
A mirror at `x_v` could move the visible point to the first rough vertex behind it, as RTX Remix's primary surface replacement does, with its own motion and guides.
Caustics — light through glass or off a mirror onto a diffuse surface — are what reconnection cannot carry.

## ReSTIR DI

**A second track, in parallel with GI phase 1** (decision 5).
It uses the same visible surface records, the same reprojection and acceptance, the same generic target function and the same final-shading entry.
What differs is the sample: a light and a point on it, rather than a secondary vertex.

- **Candidates**: 8 to 32 light samples per pixel, drawn by power-weighted selection, with `p̂` the unshadowed contribution at `x_v`; they cost no rays.
- **Reuse**: temporal and spatial as for GI.
  A light sample is a point on a light, in area measure, so its shift is the identity and its Jacobian 1.
- **One shadow ray** for the kept sample, replacing the bounce-0 light sample connect traces today.
- **The environment stays next-event estimation**; it has its own importance sampler.

With 22 lanterns in Sponza's evening variant, uniform selection gives the one that matters to a pixel a 1-in-22 chance per sample.
Resampling 16 candidates and reusing a neighbor's choice makes it close to certain.

**Power-weighted selection comes first, in both tracers**, as its candidate distribution and as the final mode's own gain (decision 4).
[lights.md](../lights.md) lists it as open.

**In final renders, later**: unbiased ReSTIR DI, with GRIS weights and every visibility ray traced, at the visible point of the converging wavefront.
A mean of unbiased frames converges to the right image however correlated they are; correlation slows it rather than biasing it.

## Final renders

Decided (decision 4): plain path tracing, plus power-weighted light selection now, and unbiased ReSTIR DI later.
ReSTIR GI, in the form built for the interactive mode, never reaches the mean: its shading at `x_s` is the stored `L_o`.
ReSTIR PT, the unbiased answer for indirect light, is not planned.

## SGL and sg

**Nothing new is needed from sg for the traced path.**
Dense compute dispatches, buffers, inline traces and indirect dispatch for the per-permutation shade all exist.
The polyfill item of decision 10 is the one sg extension the hybrid wants, and only for devices without ray tracing.

**SGL has what the passes need:**

- **Inline ray queries in compute** (`world.trace`), as extend and connect use them, with the same cutout any-hit and quadric intersection.
  They exist on dx12, vulkan and metal; webgpu emulates them, and the wavefront is out of scope there, for lack of bindless arrays.
- **Buffers of structs**, laid out as structure of arrays like the wavefront's.
- **Packing**: `pack_half2x16` and `unpack_half2x16`, and the `reinterpret_as_*` builtins.
- **Random numbers**: `pt.hash` and `pt.rand`.

**What SGL lacks, and how the plan avoids it:**

- **No float atomics and no compare-exchange.** Nothing needs them: every reservoir has one writer.
- **No arrays in GPU memory.** A reservoir is a struct of scalars and vectors, and a candidate list lives in locals.
- **No `const` arrays.** Neighbor offsets come from a low-discrepancy sequence computed in the kernel.
- **No assignment to one member of a buffer element.** A pass reads a whole reservoir and writes it whole.
- **No indirect calls.** This is why the final shading is a generated entry per permutation, and why the target function reads no material.

**The NVIDIA `InterlockedOr` driver bug**: the resampling passes use no atomics, queues or counters.
The one new user of counters is the sort of the persisted hits, which follows the wavefront's two rules ([wavefront.md](../wavefront.md#pixel-order-and-nvidias-dx12-driver)).
It joins the re-test on each new driver that the roadmap's housekeeping lists.

## Expected cost and quality

**Measured** is today's tracer, from [Why](#why); everything else is an **estimate**.

Rays per traced pixel per frame:

| work | rays |
|---|---|
| primary | 1, or 0 with raster primaries |
| next-event estimate at `x_v`, light and environment | up to 2 |
| the candidate's ray to `x_s` | 1 |
| each vertex from `x_s` on: its next-event estimate and continuation | up to 3, over the path's depth |
| temporal visibility | 1 |
| spatial visibility | `k`, 1 or 2 |
| reverse visibility, `exact` only | 1 + `k` |

Today's moving frame already traces everything above the temporal row; ReSTIR adds 2 or 3 rays per pixel, or 4 to 6 with `exact`.

**A moving frame at 640x360, for 720p, like for like** — full-depth candidates, `k = 2`:

| part | GPU time |
|---|---|
| today's moving frame, which the candidates are | ~22 ms, measured |
| visibility rays: 3 per pixel, at connect's ≤ ~3–4 ns per ray in a full still round | ~2–3 ms, estimated |
| resampling arithmetic | ~1 ms, estimated |
| sorting the persisted hits | ~0.3 ms, estimated from sort's 0.8–1.5 ms at four times the pixels |
| `sv_restir_shade` per permutation | ~2–3 ms, estimated from shade's full-occupancy cost, without sampling or next-event estimation |
| **total** | **~28–30 ms**, estimated |

Beside it, CPU recording grows by ~28 dispatches and their declarations, not measured.

What follows from it:

- **ReSTIR costs ~6–8 ms more than today's moving frame**, and spends it on a signal the paper measures 9.3x–166x less noisy.
- **At 1080p**, 960x540 traced, scaling by pixels gives ~63–68 ms, which does not fit a frame on this GPU; neither does today's moving frame.
  Half-rate candidates — half the pixels trace a new path each frame, in a checkerboard, and reuse covers the rest — halve the ~22 ms part, at the price of slower response to change.
- **One-bounce GI saves the rounds after round 1**, measured at ~45% of the segments.
  Rounds 2–4 hold ~356,000 of ~795,000 segments.
  At ~45% of extend through connect's ~19–20 ms, that is ~8–9 ms saved, estimated by segment share.
  Begin, resolve, args and the drain's empty rounds stay.
- **Shade is the largest stage either way**, ~9.8 of the moving frame's 22 ms, and `sv_restir_shade` adds to it.
  Making shade cheaper pays twice here, and is a prerequisite rather than a part of this plan.

What to expect of the image, from the papers and others' practice rather than measurement:

- Indirect light from sunlit patches, Sponza's case, is where resampling gains most.
- Blotchy, correlated noise where reuse is heavy, which the denoiser has to cope with.
- Plain path-tracing noise on mirrors, smooth glass and glossy visible points.
- A slight darkening at contact and in corners from the skipped reverse visibility, which `exact` removes.
- A brightness error near rough but non-Lambertian `x_s`, bounded by the roughness threshold.

## Phasing

Two tracks over one foundation (decision 5).
Each phase lands with its tests, in sv's style: images compared block-wise against a reference, with the plain path tracer as the reference wherever one is needed.
**Every test names its variant**: the biased default with a loose bound, or `exact` with the reverse visibility traced and a noise bound.
On a scene of Lambertian surfaces `exact` is unbiased; elsewhere the Lambertian simplification remains, and a test there names its bound accordingly.
GPU tests take their time; each phase narrows the expensive ones under `nx::is_thorough()`.

### The foundation, built once

Whichever track reaches it first builds it, and the other uses it:

- **The records** on the accumulation slot, per extent, with their resets and their byte count.
- **The persisted primary hit, its sort, and the generated `sv_restir_shade` entry.**
- **Per-pass random streams.**
- **The reservoir math as pure SGL functions with `test` blocks**, which the interpreter runs without a GPU: merging, the Jacobian, the MIS weights.
  Among them a deterministic two-pixel test: for a sample shifted between two domains, the `m_i` sum to one over the shift's domain, in both directions.
- **Reprojection through the records' own camera and extent, and the acceptance test.**

### GI track

**Phase G0: the candidate, with reuse off — built.**
The throughput rebase, the direct sum, the candidate, the invalid rules, and the re-shade shading the candidate itself.
`render_settings::restir` turns it on for a layer's restarting frames that feed a temporal denoiser.
On the Cornell box the re-shade gives back each path's own estimate to ~2e-5, and the mean moves by ~2e-7.

- **The estimator is today's**: with reuse off, a Cornell box agrees with the wavefront block by block, within the noise of the sample count.
  The roulette differs, so the comparison is statistical.
- **The candidate is right**: on a box lit by one rect light, `x_s` and `L_i` read back for a pixel match a direct computation.
- **`sv_restir_shade` is the shade**: its image of the primary hits equals the round-0 shade's direct and emitted light, to rounding.

**Phase G0.5, optional: a stored reduced closure** as a measured baseline for the re-shade's cost, with its bias on `openpbr-spheres` recorded beside it.

**Phase G1: temporal reuse, on traced primaries**, behind the per-layer setting, on moving frames and on the young still frames of [the camera-stop fix](#the-camera-stops).

- **Unbiased on a Lambertian scene, `exact`**: a still camera, many frames of the resampled signal averaged, against the converged plain image; the bound is the noise of the frame count.
  The default variant, held to a looser bound, measures what skipping the reverse visibility costs.
- **The moving image has the still mean's brightness**: frames of a slow pan with full-depth candidates, their mean per block against the converged plain image, within a bound.
  The same test with one-bounce GI shows the option's darkening, and pins that it stays an option.
- **The variance falls**: per-frame error against the reference at least 3x below plain one-sample frames on the box, well inside the paper's lowest 9.3x.
- **Reprojection is pinned**: a panning camera against the same stream told nothing moved; the honest one converges further, as svgf's test does.
- **History resets where it must**: a camera cut and a scene change each leave every reservoir at `M = 1` on the next frame, read back.
- **The stop keeps its history**: the first full-size still frame after a scaled stream reads back a mean `M` above 1.
- **A disocclusion does not ghost**: a camera sliding past an occluder; the disoccluded blocks are no worse than plain path tracing.
- **An invalid candidate falls back**: a fog volume between a wall and the camera's surfaces converges to the plain image.
- A capture of Sponza, moving and at the stop, beside today's, for the user to judge; not a test, since the asset is an on-request fetch.

**Phase G2: spatial reuse**, with the reconnection Jacobian, visibility rays, pairwise MIS and the reconnection criterion at `x_s`.

- **The Jacobian against finite differences**: a probe shader maps a small cone of directions at one visible point to another, and compares solid angles.
- **A white furnace, `exact`**: a closed Lambertian scene of albedo below 1 lit uniformly has an analytic answer everywhere.
  Every sample carries the same radiance, so an error in the weights or the Jacobian shows as a wrong mean rather than as noise.
- **No light leaks through occluders, `exact`**: a wall between a lit and an unlit region; the unlit side matches the reference.
- **A glossy `x_s` is not reused**: a glossy floor seen from a diffuse wall, default variant, within a loose bound of the reference.
  A clearcoat over a rough base is not reconnected, which a readback of the shift's outcome pins.
- dx12 against vulkan, to rounding, as the raster tests do.
- **Half-resolution or checkerboarded reservoirs** are this phase's lever for 1080p, measured rather than tested.

**Phase G3: raster primaries, the hybrid**, with the work items of [Raster primaries](#raster-primaries).

- **The hybrid equals traced primaries**: the same scene, with the tracer's coverage and the same jitter, against phase G2's image, block by block.
- **What raster cannot draw is declared**: a scene with a quadric batch warns once, as the raster path does.

### DI track

**Phase D0: power-weighted light selection**, in both tracers, for every mode.

- **Unbiased**: the `lights` scene converges to the same image as with uniform selection.
- **Better**: a scene of lights of unequal power, equal samples, lower error than uniform.

**Phase D1: ReSTIR DI at the visible point**, beside GI phase G1, on the foundation.

- **Many lights**: a scene of 22 small lights, equal rays per pixel, error against the reference well below next-event estimation.
- **Unbiased, `exact`**: frames averaged against the converged plain image.

**Phase D2, later: unbiased ReSTIR DI in final renders.**

- **The mean stays unbiased**: the converging mean with it on, against the plain mean, on scenes with every kind of lobe.

## Alternatives considered

- **A radiance cache: a spatial hash grid** in the style of SHaRC (NVIDIA's RTXGI) and of path space filtering (Binder et al. 2019).
  Cheap, multi-bounce and view-independent, and biased by its cell size: light leaks across thin walls, and contact detail blurs.
  sv's product-visualization scenes live on contact detail, which is why it was not the first choice.
  **It stays a possible later complement**: a full-depth candidate could end in the cache past `x_s`, which cuts the deepest part of a candidate's cost.
  Bevy's Solari pairs ReSTIR with exactly such a cache.
- **DDGI** (Majercik et al. 2019): probe volumes of irradiance and visibility.
  Diffuse only, low frequency, prone to leaks where probes land in walls, and in need of probe placement per scene.
- **Neural radiance caching** (Müller et al. 2021): trained online on the GPU, which needs matrix hardware SGL does not reach.
  [compute-throughput.md](../../../shaped-graphics-language/docs/spec/incubator/compute-throughput.md) records what that asks of SGL.
- **Making the plain wavefront cheaper**: shorter paths, fewer drain rounds (17 of a moving frame's 22 rounds are empty in Sponza), better occupancy.
  It lowers the cost and leaves the noise per path as it is, so it complements rather than replaces, and pays in both modes.
- **RTXDI itself.** Its license is NVIDIA's RTX SDKs license, and SGL is the only shading language here.
  Translating its shaders raises the question NRD's license did, so this plan implements the published algorithms in our own SGL.
  The BSD- and MIT-licensed implementations above are fair to read and learn from.
- **ReSTIR PT.** The unbiased, all-transport answer, and several times the work and cost of ReSTIR GI; decision 4 does not plan it.

## Open questions

1. **The step that remains at the stop**: seed the full-size denoiser history from the last upscaled output, an sr extension, or keep the scaled stream through the hand-off?
   Phase G1 measures the step first.
2. **The young still frames' cost**: ~24–32 ms of full-size resampling and re-shade (estimated) on top of each round.
   If measurement confirms it, does the camera-stop fix run at full size, or at the scaled extent with the second option above?
3. **Which reservoir feeds the next frame**: taken as the temporal one while building, as the paper does, since it correlates the frames less.
   The spatial output feeding back is a tuning experiment for after phase G2.

## References

- Talbot, Cline, Egbert, *Importance Resampling for Global Illumination*, EGSR 2005.
- Bitterli, Wyman, Pharr, Shirley, Lefohn, Jarosz, *Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting*, ACM TOG 39(4), SIGGRAPH 2020.
- Ouyang, Liu, Kettunen, Pharr, Pantaleoni, *ReSTIR GI: Path Resampling for Real-Time Path Tracing*, Computer Graphics Forum 40(8), 17–29, HPG 2021.
  [Project page](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing), [Eurographics library](https://diglib.eg.org/handle/10.1111/cgf14378).
- Wyman, Panteleev, *Rearchitecting Spatiotemporal Resampling for Production*, HPG 2021, DOI 10.2312/hpg.20211281.
- Boissé, *World-Space Spatiotemporal Reservoir Reuse for Ray-Traced Global Illumination*, SIGGRAPH Asia 2021 Technical Communications.
- Lin, Kettunen, Bitterli, Pantaleoni, Yuksel, Wyman, *Generalized Resampled Importance Sampling: Foundations of ReSTIR*, ACM TOG 41(4), Article 75, SIGGRAPH 2022.
  [Project page](https://research.nvidia.com/labs/rtr/publication/lin2022generalized/), [code](https://github.com/DQLin/ReSTIR_PT).
- Bitterli, *Correlations and Reuse for Fast and Accurate Physically Based Light Transport*, PhD thesis, Dartmouth College, awarded 2022; GRIS cites it as 2021.
- Wyman, Kettunen, Lin, Bitterli, Yuksel, Jarosz, Kozlowski, *A Gentle Introduction to ReSTIR: Path Reuse in Real-time*, SIGGRAPH 2023 Courses, DOI 10.1145/3587423.3595511.
  The course notes add Giovanni De Francesco as an author.
- Kettunen, Lin, Ramamoorthi, Bashford-Rogers, Wyman, *Conditional Resampled Importance Sampling and ReSTIR*, SIGGRAPH Asia 2023.
- Sawhney, Lin, Kettunen, Bitterli, Ramamoorthi, Wyman, Pharr, *Decorrelating ReSTIR Samplers via MCMC Mutations*, ACM TOG 43(1), Article 10, 2024.
- Lin, Kettunen, Wyman, *ReSTIR PT Enhanced: Algorithmic Advances for Faster and More Robust ReSTIR Path Tracing*, I3D 2026, Best Paper.
  Published in the Proceedings of the ACM on Computer Graphics and Interactive Techniques; [project page](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/).
- Conty Estevez, Kulla, *Importance Sampling of Many Lights with Adaptive Tree Splitting*, HPG 2018, DOI 10.1145/3233305.
- Müller, Gross, Novák, *Practical Path Guiding for Efficient Light-Transport Simulation*, Computer Graphics Forum 36(4), EGSR 2017.
- Müller, Rousselle, Novák, Keller, *Real-time Neural Radiance Caching for Path Tracing*, ACM TOG 40(4), SIGGRAPH 2021.
- Majercik, Guertin, Nowrouzezahrai, McGuire, *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*, JCGT 8(2), 2019.
- Binder, Fricke, Keller, *Massively Parallel Path Space Filtering*, arXiv:1902.05942, 2019.
- NVIDIA, [RTXDI](https://github.com/NVIDIA-RTX/RTXDI), with its [ReSTIR GI notes](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md).
- JMS55, [Realtime Raytracing in Bevy 0.17 (Solari)](https://jms55.github.io/posts/2025-09-20-solari-bevy-0-17/), 2025.
- US 11,922,556 B2, *Ray-traced light resampling using screen space visibility*, NVIDIA, inventor Alexey Panteleev, granted 2024-03-05.
