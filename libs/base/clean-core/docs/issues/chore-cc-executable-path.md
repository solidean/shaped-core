---
created: 2026-11-05
refs:
  - file: libs/base/nexus/src/nexus/source_root.cc
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_runtime.cc
---

# No clean-core query for the running executable's path

Two places ask the platform for the running executable's path, each in its own way.
`nx::source_root` walks up from it to the build's `CMakeCache.txt`, with a branch per platform (GetModuleFileNameW, `/proc/self/exe`, `_NSGetExecutablePath`, node's script).
dx12's `agility_directory` asks GetModuleFileNameW again, and keeps only ASCII.

Wanted: one `cc::executable_path()` among clean-core's system queries, which both would call.
Not done with the source-root change, which kept to nexus.
