---
created: 2026-11-05
priority: normal
tags: [input]
parent: epic-xr
claimed-by: Claude (for Philip)
claimed-at: 2026-11-06T12:44Z
---

# Gamepads in sr, from SDL3

Decided in `epic-xr`: gamepads belong to sr's window input, and there is no shared action layer with XR for now.
SDL3 already has the gamepad API, so this is events and state in `sr/input.hh`.
