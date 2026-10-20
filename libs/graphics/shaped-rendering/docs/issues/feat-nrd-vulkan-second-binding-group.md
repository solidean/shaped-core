---
created: 2026-10-17
refs:
  - symbol: sr::nrd_denoise_routine
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/nrd_session.cc
  - file: libs/graphics/shaped-rendering/docs/reconstruction.md
  - file: extern/nrd/CMakeLists.txt
---

# NRD on vulkan: the caller's half

`sr::nrd_denoise_routine` is dx12-only, and that is a scope call rather than a property of NRD.
[reconstruction.md](../reconstruction.md) has the whole story; this is the part that belongs to sr.

Turning on NRD's embedded SPIR-V is one line: `NRD_EMBEDS_SPIRV_SHADERS ON` plus `SHADERMAKE_DXC_VK_PATH` produces 31 SPIR-V blobs from the DXC `extern/dxc` already pins.
Built that far and run against a validating vulkan context, two things broke:
- **NRD uses two register spaces, which are two descriptor sets under Vulkan.**
  `nrd_session::create` builds one binding group, which is one set, so the constants land outside the layout it declares.
  That half is sr's, and what this issue is: a second binding group here.
- **NRD's shaders want compute derivatives.**
  That half was sg's and has landed: the vulkan backend now probes and enables `VK_KHR_compute_shader_derivatives` (f71c9f5b8).
  Not yet re-run against NRD's SPIR-V.

**With validation off the same run passes**, which is worth knowing before anyone reads a green run as support.
