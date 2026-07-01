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
- **Backend registration:** `sg::create_context(backend_kind)` is an unwired stub — decide how
  the core selects/loads a backend without inverting the sg-core ← backend dependency (today a
  caller constructs `sg::context` from an explicit backend context).
- **SDK detection:** dx12/vulkan backends are pure stubs with no SDK dependency; add real
  `find_package(Vulkan)` / Windows-SDK detection and availability gating when (2) lands.
- **Tier 2 / legacy backends:** metal, webgpu, then opengl, webgl.
