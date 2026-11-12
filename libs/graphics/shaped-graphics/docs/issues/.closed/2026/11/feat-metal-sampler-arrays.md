---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_binding_create.cc
  - test: "sg - an array of samplers binds each element, through a group and staged in place"
closed: 2026-11-06
resolution: fixed
---

# Arrays of samplers on metal

`sampler[N]` and `comparison_sampler[N]` bind on dx12 and vulkan since abe3cc4c0; sg's metal backend refuses one at layout creation (`metal_binding_create.cc`).
The sampler test skips on metal until it binds them.

Hypothesis, not tried: a Metal argument buffer holds a sampler array as consecutive `[[id(n)]]` slots, so each element is written at its slot plus the element, as dx12 and vulkan do.
The sampler state needs `supportArgumentBuffers` set when it is created, which the existing single-sampler path should already do.

## Resolution

It was the backend alone: SGL already declared the array as consecutive `[[id(n)]]` slots, and the sampler states were already argument-buffer capable.
A group now writes each element at its binding's slot plus the element, and refuses a missing, repeated or out-of-range element by name.
A staging snapshot also leaves sampler bindings out of its resource arrays, where an array of them was reported as a bound array with no declared access.
Philip's sampler test runs on metal, and `sg metal - a sampler array is supplied element by element, each once and every one` pins the refusals.
