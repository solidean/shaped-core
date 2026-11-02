---
created: 2026-10-17
blocked-by: [feat-slib-package-include-dirs]
refs:
  - file: extern/nrd/CMakeLists.txt
---

# Configure copies NRD's shader headers into shaped-rendering's source tree

[extern/nrd/CMakeLists.txt](../../../../../extern/nrd/CMakeLists.txt) copies `NRD.hlsli` and `NRDConfig.hlsli` into `libs/graphics/shaped-rendering/shaders/nrd/` at configure time.
It must, because a shader package cannot include from outside its `SOURCE_DIR`.
That is the only place in this repository where configure writes into the source tree.

Two costs come with it:

- Two presets configured from one checkout write the same files, so the source tree carries whichever version configured last.
- A read-only source tree cannot configure.

It goes once a package can name `INCLUDE_DIRS`.
