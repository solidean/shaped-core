---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_subgroups.cc
---

# Subgroup operations over 64-bit values

The `subgroup_*` operations take 32- and 16-bit values; dx12's wave intrinsics and vulkan's subgroup operations take 64-bit integers and doubles too.
Each needs `subgroups` beside the value's own feature, and vulkan's `shaderSubgroupExtendedTypes`.
