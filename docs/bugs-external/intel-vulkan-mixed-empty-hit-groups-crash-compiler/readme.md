# Intel Vulkan: a ray-tracing pipeline of eight hit groups or more, empty ones beside full ones, crashes the compiler

**Status:** not reduced to a standalone repro; reproduced in shaped-viewer's own test, every run; not yet filed upstream.
**Affects:** Intel Arc B580, Vulkan driver 1663934 (101.9150), Windows 11.
The AMD Radeon iGPU's Vulkan driver builds the same pipelines.
**Found by:** `sv vulkan - every mode and every denoiser draws the view it draws on dx12`, which ended in an access violation inside `igc_default64` under `vkCreateRayTracingPipelinesKHR`.
shaped-viewer's issue bug-sv-vulkan-denoiser-modes-crash-intel-compiler is the history.

## What happens

The viewer's megakernel pipeline is a raygen, two misses, and ten triangle hit groups.
They alternate a group with the viewer's closest hit, about 180 to 200 KB of SPIR-V each, and an empty group, which a shadow ray takes.
Trial creates of parts of that one pipeline, on the Arc, each in the same process before the real one:

```
hit groups                                                    result
------------------------------------------------------------  -------
empty groups alone, up to 12                                  builds
one closest hit in each of 9 groups                           builds
8 groups of the 5 closest hits, none empty                    builds
the 5 closest hits, then one empty group                      builds
one closest hit, then 6 empty groups                          builds
one closest hit, then 7 empty groups                          crashes
4 pairs of a closest hit and an empty group                   crashes
the whole pipeline, its empty groups given a closest hit      builds
that returns at once
```

Serializing every ray-tracing create changes nothing, and neither does leaving the pipeline cache out.
So it takes eight or more hit groups, some empty and some not; how many of each is not settled past that.

## Why it is probably not our bug

The modules pass `spirv-val`, the validation layer reports nothing, and the crash is inside the driver's compiler, not in a call that returns an error.
A no-op closest hit is no change to what the pipeline computes, and with it the same pipeline builds.

## What is not settled

A standalone host builds every shape above with trivial shaders: a raygen tracing one ray, a miss, and a closest hit writing the payload.
So the viewer's large closest hit is part of the trigger, and a repro would need its module and the layout it binds.

## What we do about it in the tree

`vulkan_raytracing_pipeline.cc` gives an empty triangle hit group a closest hit that returns at once, on an Intel GPU alone.
It leaves the payload as no closest hit would, so a shadow ray's answer is unchanged; the cost is a call per hit for those rays.
Without it, the agreement test crashes within seconds on the Arc.

## What to re-check after a driver update

Set `fills_empty_groups` false in `vulkan_raytracing_pipeline.cc` and run the test above with `--thorough`; once it passes, the workaround may go.
