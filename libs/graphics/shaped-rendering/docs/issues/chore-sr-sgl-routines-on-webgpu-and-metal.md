---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/CMakeLists.txt
---

# Hold webgpu and metal to sr's SGL routines

Every routine's own shaders are SGL; only `nrd_repack` and `nrd_resolve` stay HLSL, around NRD's own `NRD.hlsli`, whose license allows no translation.
The native denoise members, the network, the compute box-filter mipmap and FSR's clears build on every backend.
They are tested on dx12 and vulkan alone, since their tests still sit in the DXC-gated list.
Moving those tests to the SGL list is what would hold webgpu and metal to them.
The box filter's 1D array is the case to watch there: WGSL has no 1D array texture, and SGL writes it as a 2D array one.
