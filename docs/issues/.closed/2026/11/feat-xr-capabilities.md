---
created: 2026-11-06
priority: next
tags: [xr, ar]
parent: epic-xr
closed: 2026-11-06
resolution: fixed
---

# Capabilities: asked for on the system, reported per session

The seam every AR feature plugs into, designed in [shaped-xr's design](../../../../../libs/graphics/shaped-xr/docs/design.md#ar-capabilities).

## Wanted

- `sxr::capability`: passthrough, environment depth, scene planes, scene meshes, anchors, persistent anchors, hit tests.
- `system_description::wants`, a list replacing `wants_passthrough`, because extensions must be named before the instance exists.
- `system::offers(capability)`, replacing `offers_passthrough`, which is `yes` only where the runtime lists the extension and the system says it supports it.
- `sxr::availability`: `unsupported`, `needs_permission`, `denied`, `available`, from `session::availability_of(capability)`.
  A user can refuse `USE_SCENE` on the Quest, which `sg::context::supports` has no way to say.
- `session_description::passthrough` stays until `feat-xr-passthrough` turns it into a blend mode.

## Testing

Which extensions a capability needs is a pure mapping and is tested without a runtime, as `impl/view_math.hh` is.
