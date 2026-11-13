---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/hybrid.sgl
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_common.cc
---

# The hybrid fragment shader takes Metal 15 s and over 500 MB to build, per variant

On an Apple M4, a cold Metal shader cache, the `sv_hybrid` fragment stage is a metallib of some 500 KB.
Building a render pipeline from it takes Metal's backend compiler 14.7 to 16.1 s, measured around `newRenderPipelineState`, eight variants in one run (`hybrid_vs` and `hybrid_quadric_vs`).
macOS logs the compiler service passing its 500 MB soft memory limit during those builds (non-fatal; the kernel takes a corpse, which may add to the time).
Every compile over 5 s in a day of logs was this shader; the rest of sv builds in under 1 s.

The metal backend builds pipelines one at a time under the process-wide `pipeline_compilation_lock()`, since concurrent MTL4 builds crash the driver.
So a cold hybrid mode adds no samples for minutes: 141 s and 33,422 frames in the content agreement test.
A user sees stand-ins that long after any change to SGL's MSL emission, which leaves the cache cold, or on a first launch.

What is not known: whether the cost is the material set inlined into one fragment, the inline ray queries, or something else; nothing was profiled yet.
Measured on 2026-11-13 by timing around the pipeline build and listing the compiler service's own log (`log show`, predicate `process == "MTLCompilerService"`).
To reproduce cold, set `$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/<device folder>` aside for a run and put it back after.
