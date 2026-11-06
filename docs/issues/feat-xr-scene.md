---
created: 2026-11-06
priority: normal
tags: [xr, ar]
parent: epic-xr
blocked-by: [feat-xr-capabilities]
---

# Scene: planes and meshes as an immutable snapshot

Design: [AR capabilities](../../libs/graphics/shaped-xr/docs/design.md#ar-capabilities).

## Wanted

- `session::query_scene`, returning `cc::shared_async<sxr::scene>`, completed from `poll_events`.
- `sxr::scene_entity`: id, pose in tg's frame, label, raw label, optional plane shape, optional mesh shape.
- `sxr::scene_label`, our enum with `other`, keeping the runtime's raw string.
- A change event, on which an app queries again.
- `session::request_room_scan`, since Quest's planes come from Meta's Room Setup and an unscanned room is an empty scene.
- Written against `XR_FB` and `XR_META`, with the public shape following `XR_EXT_spatial_*`: capabilities, async discovery into a snapshot, entities holding components.
  EXT is added once Quest has plane tracking there.

## Grow-vs-work-around

tg's planes and meshes should carry the shapes where they fit, and what is missing there belongs in tg rather than here.
