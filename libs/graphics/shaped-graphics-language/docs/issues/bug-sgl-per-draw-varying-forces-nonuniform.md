---
created: 2026-10-19
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - file: libs/graphics/shaped-viewer/shaders/sgl/raster_shade.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
---

# A per-draw value reaching the pixel stage as a varying must be indexed `nonuniform`

CHK-283 makes every value a stage is handed non-uniform, so CHK-300 refuses it as a binding-array index unless it is marked `nonuniform`.
That is right for a value that varies per pixel, and wrong for one that is the same for every pixel of a draw.

shaped-viewer's raster pixel stage is the case that found it.
`p.instance` is a flat varying the vertex stage copies from `raster.draw.instance`, an inline constant, so it is one value per draw.
Every load the material makes through it is still written `tracer.bindless.buffers[nonuniform block]`, and HLSL gets 51 `NonUniformResourceIndex` in one pixel shader.
The text is correct, but `NonUniformResourceIndex` keeps a load off the uniform path, and on some hardware it is a loop per load.

The generator spells the material body once for a traced hit, a wavefront shade kernel and a raster pixel (`body_spelling` in shader_generator.cc).
At a hit the mark is needed: the lanes of a wave hit different instances.
So two fixes, which do not exclude each other:

- **In SGL:** a way to say a stage input is the same across a draw — a flat varying the vertex stage wrote from uniform values, or a declared attribute — so CHK-283 leaves it uniform.
  Vulkan defines dynamic uniformity over the invocations of one draw command, which is what makes per-draw uniformity enough for an index.
- **In shaped-viewer:** the pixel stage reads the instance from `raster.draw` directly, which the pass already proves uniform, and the raster spelling drops `nonuniform`.
  Its `needless-nonuniform` warning would then also guard the spelling.

It is not what triggers [intel-d3d12-cached-pso-draws-nothing](../../../../../docs/bugs-external/intel-d3d12-cached-pso-draws-nothing/readme.md).
With every `NonUniformResourceIndex` removed, that bug still reproduces.
