---
created: 2026-11-06
priority: normal
tags: [xr, ar]
parent: epic-xr
blocked-by: [feat-xr-capabilities]
---

# Hit sources: a ray against the room, read per frame

Done when the controller places `xr-cube` on a real table.
Design: [AR capabilities](../../libs/graphics/shaped-xr/docs/design.md#ar-capabilities).

## Wanted

- `session::create_hit_source`, from a pose action or a fixed ray.
- `frame.hits(source)`, each a pose and a `hit_status`, evaluated by `wait_frame` at display time.
- Natively over `XR_META_environment_raycast`, keeping its statuses: a ray out of the cameras' view reports that, not a miss.

## Decided

Sources are the only public shape, with no direct `raycast(ray)`: WebXR can only do sources, and a source can be built on a direct call but not the reverse.
