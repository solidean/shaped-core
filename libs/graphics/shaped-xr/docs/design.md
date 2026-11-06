# shaped-xr design

What the library is today, the decisions behind it, and what comes next.
The epic is [epic-xr](../../../../docs/issues/epic-xr.md); what the first prototype taught is on [feat-shaped-xr-core](../../../../docs/issues/.closed/2026/11/feat-shaped-xr-core.md).

## What exists

- **`sxr::system`** is the OpenXR instance and the headset it found.
  It hands out the context the runtime dictates: on vulkan the runtime creates the instance and the device itself, through `vulkan_config`'s hooks; on dx12 it names the adapter by LUID.
- **`sxr::session`** is the runtime's frame loop over that context.
  It begins when the runtime is ready and ends when it stops, as a headset taken off does, and resumes on the next ready.
- **A frame is one image of a layer an eye**, color and depth, so view instancing draws both eyes in one pass.
  Depth reaches the runtime too, where it offers `XR_KHR_composition_layer_depth`.
- **Actions** are OpenXR's: declared by name with suggested bindings per interaction profile, synced by `wait_frame`, read by name and hand.
  `sxr::standard_controller_actions()` is the usual set for Quest Touch and the simple controller, and poses come back in tg's frame.
- **Capabilities** are named in `system_description::wants`, and `system::offers` answers whether the headset has one.
  What is missing is left out rather than failing the system, and `session::availability_of` is what a session may use.
- **`sxr::controller_routine`** draws each hand as a plain box and its aim ray as a thin one, inside the frame's own pass.
- **`sxr::inspect_runtime`** reports what the active runtime lists and what the system says it supports, with no graphics binding and no session; `xr-probe` prints it.
- **`sxr::blend_mode`** is how a frame meets the room: opaque, alpha blend or additive, and `session::blend` says what came up.
  A camera layer serves alpha blend ahead of the runtime's own, since that layer under an opaque frame is what ran over Quest Link.
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

## AR capabilities

Decided on 2026-11-06; capabilities exist and nothing else here is built yet, each being one issue under [epic-xr](../../../../docs/issues/epic-xr.md).
What each platform offers comes from the pinned OpenXR headers and Meta's docs.

- **Capabilities are asked for on the system and reported per session** (`feat-xr-capabilities`).
  Extensions must be named before the instance exists, so `system_description::wants` replaces `wants_passthrough`.
  `system::offers` is what the runtime has, and `session::availability_of` is what this session may use: `unsupported`, `needs_permission`, `denied` or `available`.
  It is an enum where sg's `supports` is a bool, because a user can refuse `USE_SCENE`, and no one refuses a GPU feature.
- **The Quest backend is written against `XR_FB` and `XR_META`, and the public API follows `XR_EXT_spatial_*`.**
  EXT is the cross-vendor standard, and its model is the one to copy: capabilities, async discovery into a snapshot, entities holding components.
  Quest's support for it was unconfirmed when this was decided, so EXT is added once Quest has plane tracking there.
- **Environment depth is frame data plus an SGL module** (`feat-xr-environment-depth`).
  Meta's depth map comes with its own pose and fov per eye, so a shader must reproject into it, and a plain texture would not be enough.
  The exported module owns that, the encoding and the soft edge, so occlusion is one line in an app's material, as a Slug shape is.
- **Scene is an immutable snapshot through `cc::shared_async`** (`feat-xr-scene`), requeried on a change event.
  Discovery is async in OpenXR, and WebXR's live per-frame set can produce snapshots too, while the reverse would not hold.
  A label is our enum with `other`, keeping the runtime's raw string.
- **Hit tests are sources read per frame** (`feat-xr-hit-sources`).
  `XR_META_environment_raycast` is a direct call and WebXR's is a source; a source can be built on a direct call, not the reverse.
  Native's statuses survive in `hit_status`, since a ray out of the cameras' view is not a miss.
- **Anchors stop at one headset** (`feat-xr-anchors`): created, located, persisted to an id the app stores, restored from it.
  Sharing between headsets has its own account and network story.
- **WebXR is a constraint on shapes and not a schedule.**
  The WebXR/WebGPU binding had not shipped as far as we could find, and sg has no webgl, so `feat-xr-webxr` is parked.

The order is depth, hit sources, scene, anchors, and `question-xr-link-ar-probe` may put the APK ahead of them.

## Planned

- Hand tracking (`feat-xr-hand-tracking`).
- Quest standalone: the same code in an APK (`feat-android-apk`).
- Headless CI against Monado (`chore-xr-monado-ci`).
