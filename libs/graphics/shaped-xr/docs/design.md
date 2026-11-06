# shaped-xr design

What the library is today, the decisions behind it, and what comes next.
The epic is [epic-xr](../../../../docs/issues/epic-xr.md); what the first prototype taught is on [feat-shaped-xr-core](../../../../docs/issues/.closed/2026/11/feat-shaped-xr-core.md).

## What exists

- **`xr::system`** is the OpenXR instance and the headset it found.
  It hands out the context the runtime dictates: on vulkan the runtime creates the instance and the device itself, through `vulkan_config`'s hooks; on dx12 it names the adapter by LUID.
- **`xr::session`** is the runtime's frame loop over that context.
  It begins when the runtime is ready and ends when it stops, as a headset taken off does, and resumes on the next ready.
- **A frame is one image of a layer an eye**, color and depth, so view instancing draws both eyes in one pass.
  Depth reaches the runtime too, where it offers `XR_KHR_composition_layer_depth`.
- **Actions** are OpenXR's: declared by name with suggested bindings per interaction profile, synced by `wait_frame`, read by name and hand.
  `xr::standard_controller_actions()` is the usual set for Quest Touch and the simple controller, and poses come back in tg's frame.
- **Passthrough** is the runtime's own camera layer (`XR_FB_passthrough`) under the frame, which blends over it by alpha.

## Decisions

- **The public headers name no OpenXR, Vulkan or D3D12 type.**
  The API exists in every build, and `system::try_create` fails where there is no loader, no runtime or no headset.
- **Views are in tg's frame, converted at the edge.**
  OpenXR is right-handed with -z forward; a view is a `tg::rigid_transform3f` and a left-handed zero-to-one projection, and nothing above the session sees OpenXR's frame.
  `impl/view_math.hh` is the conversion, which the tests pin without a runtime.
- **On vulkan sg renders into the runtime's images**, wrapped with `wrap_vulkan_texture` in the attachment layouts the runtime hands them over in, and handed back with `ensure_layout`.
- **On dx12 sg renders into images of its own and copies**, from a `dx12_native_scope` with legacy barriers.
  An OpenXR D3D12 image rests in a legacy state, `RENDER_TARGET` or `DEPTH_WRITE`, which sg's enhanced barriers never meet.
- **Submission order on the shared queue is the synchronization**: the frame's list is submitted before the images are released and the frame ends.

## Planned

- Drawing the controllers, an sr-backed routine (`feat-xr-controller-actions`), and hand tracking (`feat-xr-hand-tracking`).
- Quest standalone: the same code in an APK (`feat-android-apk`).
- Headless CI against Monado (`chore-xr-monado-ci`).
