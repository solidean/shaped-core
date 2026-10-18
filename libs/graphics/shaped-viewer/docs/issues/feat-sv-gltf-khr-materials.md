---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/gltf_import.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/asset_materials.cc
---

# The glTF `KHR_materials_*` extensions do not cross into OpenPBR

babel records them but does not interpret them.
This is the one that costs something today.
Targeting OpenPBR instead of `pbr` was justified by transmission, ior, clearcoat and sheen mapping onto `transmission_*`, `coat_*` and `fuzz_*` natively.
None of them cross, so that payoff is still unrealised.
A glTF import reports what it dropped, so the gap is visible rather than silent.
