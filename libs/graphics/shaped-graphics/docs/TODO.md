# shaped-graphics TODO

Running list of known follow-ups. Bigger design intent lives in
[structure.md](structure.md).

- **First milestone:** command-list buffer upload / download / copy (sg core API + real dx12 and
  vulkan implementations). Everything in the core is currently a `CC_UNREACHABLE` stub.
- **`cc::shared_ptr`:** the `*_handle` typedefs use `std::shared_ptr` as a placeholder. Surface a
  `cc::shared_ptr` in clean-core and switch handles to it (keeps sg off `std::`). See the
  [coding-guidelines](coding-guidelines.md) note.
- **`cc::flags`:** `buffer_usage` uses a hand-rolled `enum class` + bitwise operators; migrate to
  `cc::flags` once that clean-core type is implemented.
- **Blessed escape hatch:** add an sg API that returns raw underlying GPU handles without exposing
  the concrete backend types, so callers don't reach for `dynamic_cast` to a `sg::backend::*` type.
  See the [coding-guidelines](coding-guidelines.md) escape-hatch note.
- **SDK detection:** dx12 now links the Windows-SDK D3D12 libs (`d3d12 dxgi dxguid`) directly off
  the default lib path — good enough on the gated Windows path, but there's no explicit SDK
  presence/version check yet. vulkan is still a stub; add `find_package(Vulkan)` + availability
  gating when it gets a real implementation.
- **Tier 2 / legacy backends:** metal, webgpu, then opengl, webgl.
