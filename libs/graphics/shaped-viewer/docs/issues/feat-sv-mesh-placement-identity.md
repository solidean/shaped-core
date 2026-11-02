---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/refs.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/stable_id.hh
  - symbol: sv::scene_ref::add_mesh
---

# A scene item has no stable identity

Lights and views carry a `stable_id<Tag>`, and a mesh carries only a name, which is a label.
A mesh is a resource placed any number of times, so the identity belongs to the placement.
Picking is the first thing that will need it, and object motion vectors need it too.
It is an added `add_mesh(id, mesh)` overload rather than a redesign.
