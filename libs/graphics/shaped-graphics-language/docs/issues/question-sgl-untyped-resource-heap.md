---
created: 2026-11-22
tags: [porting-feedback, webgpu]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# Should SGL have an untyped resource heap, as HLSL's ResourceDescriptorHeap is, when WebGPU has none

HLSL on Shader Model 6.6 has one untyped heap: `ResourceDescriptorHeap[i]` is whatever resource the host put at `i`, and the shader says at each use what it reads it as.
An SGL binding array is typed instead, one element type and one access mode a member, `maps: texture_2d[float4][N]`.

A port of a bindless renderer covered its one heap with a member per type and access, each aliasing the same heap range: a read and a written `float3` buffer were two members.
It declared each with a length of a million rather than `T[]`.
An unbounded array is its binding's last member (CHK-490), and a pipeline binds at most 16 of them, one to a group (CHK-491), because D3D12 and vulkan size only a group's last binding.
So eight typed views of one heap would have taken eight groups.

**WebGPU has no form for an untyped heap, nor for an unbounded array**, which is why this is a question and not a feature.
The other targets do:

- dx12: `ResourceDescriptorHeap` and `SamplerDescriptorHeap`, native on Shader Model 6.6.
- vulkan: `VK_EXT_mutable_descriptor_type`, one binding of several descriptor types, or the descriptor-buffer and descriptor-heap extensions.
- metal: argument buffers, whose entries a shader reads as the type it names.

What to decide:

- Whether an untyped heap belongs in SGL at all, given every program using it is one WebGPU can never run, or whether typed arrays over one host heap stay the one form.
- If it does: a feature WebGPU lacks, as `binding_arrays` is; how a use names the type it reads, and what checks that a host put that type there.
- Whether the smaller step serves the port as well: several typed unbounded views of one heap, each its own group on dx12 and vulkan, without the last-member rule binding them to one group each.
- What the typed form costs today: a member per type and access is verbose but explicit, and nothing in the port measured it slower.
