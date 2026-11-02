---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/oidn_network.cc
  - file: libs/graphics/shaped-rendering/CMakeLists.txt
  - symbol: sr::oidn_denoise_routine
  - test: sr - tza*
---

# OIDN weights are found through a compile-time source-tree path

The OIDN weights are found through a compile-time path, which is fine for a test and not for a shipped binary.
`SR_OIDN_WEIGHTS_DIR` points into the source tree.
The member will want the blob staged beside the executable, the way OIDN's own runtime is, or embedded.
