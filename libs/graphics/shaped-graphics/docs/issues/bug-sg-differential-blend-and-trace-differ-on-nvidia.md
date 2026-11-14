---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus
  - file: libs/graphics/shaped-graphics/tests/differential/references
---

# On NVIDIA's RTX 5080, the differential corpus's darkest blend and downward trace differ from their references

What is wrong or wanted, and what is already known.

On the desktop's NVIDIA GeForce RTX 5080, `shaped-graphics-test` fails 20 checks in the differential corpus.
Two of its cases fail, on dx12 and on vulkan alike, through DXC and through SGL's own binary alike, every route with the same values:

- `blend_equations.sgl` `blended_ps`, against `blend_equations.ref`: "darkest: 16 of 64 referenced elements beyond relative 0.005".
  Each reads 0.33333334 (0x3eaaaaab) where the reference holds 0.6666667 (0x3f2aaaab), e.g. elements 51, 119, 187 and 255.
- `scene_traces.sgl` `downward`, against the interpreter: "hits: 8 of 2048 elements" and "ids: 8 of 2048 elements" beyond relative 1e-04.
  Each reads -1 (0xbf800000), a miss, where the interpreter has a hit, e.g. a hit distance of 4.8231535 and an id of 24.
  Against `scene_traces.ref` the same case fails at 1 of 64 referenced elements, for both hits and ids.

They reproduce on origin/main 09432cabb alone, without jn/sgl-sm66-floor, built in a worktree of its own on 2026-11-14:

```
uv run dev.py test shaped-graphics-test --test-args differential
```

That every route and both APIs give the same values points at the device or the references, not at a compiler.
f6e7545c5 ("the differential matrices bound their routes by EVAL-6, as AMD's routes differ there") says the corpus "runs whole on AMD's iGPU", so it was likely only checked on AMD so far.
Earlier runs of the binary on this machine used the AMD Radeon iGPU and crashed there; this run used the NVIDIA GPU.

Not yet known: whether the references or the GPU are wrong in each case, and whether other NVIDIA GPUs agree.
