---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/mesh_attribute.hh
  - symbol: sv::attribute_format
---

# `mesh_attribute` cannot hold a struct

`attribute_format` is a scalar plus a dimensionality, so an array of PBR values has to be authored as one attribute per field.
A struct protocol — a field list of scalar/vector members, so one attribute carries one AoS payload — would let a caller hand over the array they already have.
