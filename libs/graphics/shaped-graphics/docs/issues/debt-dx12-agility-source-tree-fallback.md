---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_runtime.cc
  - file: libs/graphics/shaped-graphics/backends/dx12/CMakeLists.txt
---

# The dx12 backend falls back to an Agility runtime path compiled in from the source tree

Where no `D3D12/` directory is staged beside the executable, the backend loads the Agility runtime from `SG_AGILITY_RUNTIME_DIR`.
That is the fetched install's absolute path, compiled in, so a binary depends on where its checkout sits.
It is defined on `dx12_runtime.cc` alone, so the other files of the backend compile the same in any checkout; that file does not.
The `abspaths` gate allows it until this closes.

Wanted: no path of the source tree in the backend.
Staging `D3D12/` beside every executable that links the backend is the shipped shape already, and the fallback would then go.
Staging costs a copy of the runtime per binary directory, so it is measured before it lands.
