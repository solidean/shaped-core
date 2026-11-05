---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
blocked-by: [feat-android-apk, feat-sg-external-device-requirements, feat-sg-wrap-native-texture]
---

# shaped-xr: the library, its system, session and frame

The new library `libs/graphics/shaped-xr`, namespace `xr`, on top of sr (decided in `epic-xr`).

## Wanted

- The library skeleton: CMake, recording domain, cheat sheet, and docs carrying the decisions from `epic-xr`.
- The OpenXR loader fetched under `extern/`; `XR_HAS_OPENXR` (1/0), with the API always present and `try_create` failing without a backend.
- `xr::system`: the runtime's graphics requirements, and a context created from them as convenience.
- `xr::session`: a blocking `wait_frame` on native, and views as `tg::rigid_transform` plus `tg::perspective_off_center`.
  OpenXR's right-handed space is converted to tg's convention at the edge.
- Projection layers with depth (`XR_KHR_composition_layer_depth`), over wrapped sg images.
- OpenXR on vulkan, on Android first.
