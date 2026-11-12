---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
blocked-by: [feat-sg-external-device-requirements, feat-sg-wrap-native-texture]
closed: 2026-11-06
resolution: fixed
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
- OpenXR on vulkan, on Windows over Quest Link first; the Android port (`feat-android-apk`) reuses the same code.

## What the Link prototype taught

`feat-xr-link-cube-prototype` put the SGL cube in a Quest 3 over Quest Link on 2026-11-05, on dx12, and these held there.

- **The runtime's requirements feed the context directly.**
  `xrGetD3D12GraphicsRequirementsKHR`'s LUID, read as `LowPart | HighPart << 32`, and its minimum feature level go straight into `dx12_config`.
  `device()` and `queue()` are the whole graphics binding.
- **Submission order on the shared queue is the only synchronization needed.**
  sg's list is submitted before `xrReleaseSwapchainImage` and `xrEndFrame`, and the runtime composes after it, with no fence of ours.
- **A native copy into the runtime's image works beside sg's enhanced barriers.**
  The image is in `RENDER_TARGET` when acquired and must be again when released.
  Legacy barriers on it inside a `dx12_native_scope` raised nothing in the debug layer.
- **tg's left-handed frame needs one flip at the edge.**
  A pose converts by negating the position's z and the quaternion's x and y.
  The eye is `perspective_off_center_lh_zero_to_one` with each angle's tangent times the near distance, and the cube stood still in the room.
- **The session cycles whenever the headset comes off.**
  `FOCUSED` goes through `VISIBLE` and `SYNCHRONIZED` to `STOPPING`, where the app ends the session, then `IDLE`, and `READY` again when it is worn.
  The loop must keep polling events through all of it.
- **Link's numbers**: 1824 x 1968 recommended per eye, paced at about 86 to 90 frames a second while focused.
- **The runtime is chatty on stderr**: `VirtualLock` and `InputModalityManifestState` warnings from its IPC layer, which are harmless and outside our recording.
