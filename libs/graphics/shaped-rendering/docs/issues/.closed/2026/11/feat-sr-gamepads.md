---
created: 2026-11-05
priority: normal
tags: [input]
parent: epic-xr
closed: 2026-11-06
resolution: fixed
---

# Gamepads in sr, from SDL3

Decided in `epic-xr`: gamepads belong to sr's window input, and there is no shared action layer with XR for now.
SDL3 already has the gamepad API, so this is events and state in `sr/input.hh`.
