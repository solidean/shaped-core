---
created: 2026-11-05
priority: normal
tags: [xr, input]
parent: epic-xr
---

# Controller input as OpenXR-style actions

## Wanted

- Actions declared by name — bool, float, vec2, pose, haptic output — with suggested bindings per interaction profile.
- Action state read per frame, and poses located at the predicted display time.
- Drawing the controllers, an sr-backed routine in shaped-xr.

## Status

Actions are in shaped-xr since 2026-11-05: declared by name with suggested bindings per profile, synced by `wait_frame`, read by name and hand.
`sxr::standard_controller_actions()` binds the usual set for Quest Touch and the simple controller.
`xr-cube` grabs its cube with the trigger and buzzes on the grab.
Drawing the controllers, the sr-backed routine, is not done.
