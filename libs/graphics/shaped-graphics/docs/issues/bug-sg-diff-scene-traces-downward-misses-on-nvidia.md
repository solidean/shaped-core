---
created: 2026-11-14
priority: next
tags: [sgl, differential]
refs:
  - test: "sg differential - every corpus entry point computes*"
---

# scene_traces' downward trace reports no hit on NVIDIA, where its reference and the interpreter see one

**The three `sg differential` corpus tests in shaped-graphics-test are red on an RTX 4090, on dx12 and vulkan alike, through DXC and through SGL's own route.**
Measured 2026-11-14; `ship`'s proof found them red at 09432cabb0ad as well, so they predate that day's clean-core and nexus work.

What fails, from the gate's log:
- `scene_traces.sgl` downward, against `scene_traces.ref`: 1 of 64 elements off in both `hits` and `ids`; element 0 is -1 where the reference holds 4.8231535 (hits) and 1 (ids).
- The same entry point against SGL's interpreter: 8 of 2048 elements off, each -1, a miss where the interpreter finds a hit.
- -1 on every backend and both routes, so the GPU traces agree with each other and disagree with both references.

Earlier the same day the failing set also named `half_math` and `blend_equations`; whether those are the same cause is not checked.

## To investigate

- Whether the downward ray's setup or the scene's acceleration structure is what changed; the recent SGL commits (nested derivatives, segmented long loops) are the first suspects, unverified.
- Whether the reference itself is stale: the interpreter agrees with it, which argues against.

## Done when

The three differential tests pass on this machine, and the cause is pinned by the corpus entry point that caught it.

## Log

2026-11-14, on an RTX 5080, in whole `shaped-graphics-test` runs: the same scene_traces failure, and `blend_equations.sgl` `blended_ps` beside it.
Against `blend_equations.ref`, "darkest: 16 of 64 referenced elements beyond relative 0.005".
Each is 0.33333334 (0x3eaaaaab) where the reference holds 0.6666667 (0x3f2aaaab), e.g. elements 51, 119, 187 and 255.
Both fail on dx12 and vulkan, through DXC and through SGL's binary, with the same values, and at origin/main 09432cabb built alone in a worktree of its own.
f6e7545c5 says the corpus "runs whole on AMD's iGPU", so it was likely only checked on AMD so far.
