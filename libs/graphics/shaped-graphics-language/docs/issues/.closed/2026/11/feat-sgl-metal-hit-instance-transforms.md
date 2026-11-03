---
created: 2026-11-02
parent: epic-sgl-metal-raytracing-dxr-parity
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/raytracing.md
closed: 2026-11-02
resolution: fixed
---

# metal: a pipeline hit's instance transforms, and a closest hit's object ray

In a metal pipeline, a hit record's object-to-world and world-to-object transforms are the identity.
A closest hit's `object_ray` is the world ray, while an intersection's is in object space.
On dx12 and vulkan both are the instance's, so a closest hit that transforms a normal by them shades an instanced mesh wrongly on metal alone.

Metal hands a traversal its instance's transforms only under the `instancing` and `world_space_data` intersection tags, and the intersection function tables sg builds do not declare them.
So the fix has two sides:

- **sg** builds its intersection function tables, and the intersector the raygen kernel uses, with those tags.
- **SGL** declares the same tags on every intersection function and on the kernel's intersector, and fills the hit record from the result's transforms.
  The closest hit's object ray then follows from the world ray and the world-to-object transform.

A test should trace an instance with a non-identity transform through a pipeline and read back the transforms and the object ray, on every target.
