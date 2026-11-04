---
created: 2026-11-04
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
  - test: sv environment estimate - exact converges to the sampled image where the sky is partly blocked
---

# The environment's variate is noisier than sampling where most of the sky is blocked

`environment_estimate::exact` adds the sky's unshadowed light through the closure and subtracts what each blocked sample would have carried.
With the variate at full strength, a hit that sees little of the sky subtracts on most of its samples, which is noisier than adding on the few that arrive.
Measured with the diffuse half alone, in the open Cornell box under the studio sky, which sees the sky only through its front, as the relative difference between two converged halves:

| mode       | sampled | exact  |
|------------|---------|--------|
| megakernel | 0.0335  | 0.0399 |
| wavefront  | 0.0307  | 0.0391 |
| forward    | 0.0619  | 0.0853 |
| deferred   | 0.0609  | 0.0852 |

A floating cube, open to the sky, goes from 0.050 to 0.0055 at one sample per pixel the other way.

Wanted: a strength per hit between 0 and 1, which stays unbiased as long as it is independent of the samples it scales.
Two candidates, neither tried:

- one extra visibility ray toward a cosine-drawn direction, its outcome choosing full strength or none;
- the visibility measured over earlier frames, per pixel, for a primary hit, which costs no ray but holds no history for a bounce.

A dome has no irradiance on the GPU yet, so its diffuse light is still sampled; `sv::dome::irradiance` exists on the CPU.
