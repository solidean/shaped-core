---
created: 2026-11-05
priority: next
tags: [xr, graphics]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/capabilities.hh
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/present/swapchain.hh
  - symbol: sg::backend::vulkan::vulkan_config
  - symbol: sg::backend::dx12::dx12_config
---

# XR: VR and AR on sg, sr and SGL, Quest 3 first

VR and AR rendering and input for sg, sr and SGL.
shaped-viewer is out of scope.

## Targets

- **The Quest 3 is the first headset, reached over Quest Link first**: a Windows app, OpenXR on Meta's Link runtime, vulkan.
- **Quest 3 standalone follows**: Android, the same OpenXR and vulkan code, packaged as an APK.
- **PCVR on dx12** and **WebXR** (webgpu) are in scope.
- **AR passthrough is a major use case**, not an afterthought.
- **Controller and hand input is needed soon.**
- visionOS (Compositor Services, metal) is later and not designed for now.

## What exists

- `sg::feature::vertex_layer_output` and SGL's `@layer` already draw both eyes into a texture array in one pass on dx12, vulkan and metal.
- tg has `perspective_off_center` and `rigid_transform`, which is all an OpenXR eye projection and pose need.
- Nothing mentions OpenXR, multiview or `SV_ViewID`.
- sg cannot wrap a native image, and `dx12_config` / `vulkan_config` cannot take a runtime's device requirements.
- The NDK presets build, but nothing packages an APK, so nothing runs on a Quest.

## Decided (2026-11-05)

- **A new library, shaped-xr** (`libs/graphics/shaped-xr`, namespace `xr`), **on top of sr**.
  It owns XR's own routines: occlusion against environment depth, controller and hand drawing, imgui panels on quad layers, a PCVR mirror window.
  The session and input core needs nothing from sr, so it may split into its own target later if a poses-only consumer appears.
- **The runtime's graphics requirements are the primitive; shaped-xr creating the context is convenience on top.**
  `xr` hands out what the runtime demands — adapter, extensions, and on `XR_KHR_vulkan_enable2` a device-creation hook — and an app may build the sg context from them itself.
- **A frame wait blocks on native**, like a vsync present; wasm inverts it through the existing main-loop hook.
- **SGL's stereo surface is a `@view_index` input**; the pipeline's view count says how many views.
  On webgpu it lowers to a per-draw value and a draw per view.
- **AR: all four capabilities are in the first design** — environment depth, scene planes and meshes, spatial anchors, hit tests.
  The demo app in mind uses them, so the session API is shaped around them from the start, even where they land after passthrough.
- **Input: gamepads go in sr from SDL, and there is no shared action layer for now.**
  Window input stays in sr; XR input is OpenXR-style actions in shaped-xr.
- **Quest packaging is a gradle-free APK built in CMake**, with `dev.py` deploying and launching over adb.
- **CI tries Monado headless**; the Meta XR Simulator and the Quest are for development by hand.
  Whether GitHub runners can host Monado is unknown, and not having it there is acceptable.
- **Link before standalone, vulkan before dx12** (decided later the same day).
  Link gives the Windows loop — debugger, RenderDoc, hot reload — with no APK.
  Vulkan first keeps the Link code identical to the standalone code: the same `XR_KHR_vulkan_enable2` binding, the same multiview.
  What Link hides is Adreno's performance profile, which standalone brings back.
  Whether environment depth, scene and anchors reach Link is unverified; `question-xr-ar-capability-api` checks it.

## Still to settle

- **Headset images are wrapped sg textures**, not an `sg::swapchain`: the frame is wait / begin / end, not acquire / present.
  Proposed and not contested; it becomes settled with the sg hooks.
- **View instancing is phase one**, because on Adreno multiview is the only fast stereo: an sg feature beside `vertex_layer_output`, a `view_count` on pass and pipeline.

The design deck, its answers and annotations are in `.tmp/xr-design/` for now; what is decided here moves into shaped-xr's docs when the library starts.

## Phases

1. sg hooks: `feat-sg-external-device-requirements`, `feat-sg-wrap-native-texture`, `feat-sg-view-instancing`, with `feat-sgl-view-index` beside them.
2. shaped-xr core on OpenXR and vulkan, on Windows over Link: `feat-shaped-xr-core`, then `chore-xr-monado-ci`.
3. One SGL source, multiview, in the headset: `feat-xr-cube-example`.
4. `feat-xr-passthrough` and `feat-xr-controller-actions`; the AR capability API is designed first, in `question-xr-ar-capability-api`.
5. Standalone on the Quest: `feat-android-apk`.
6. `feat-xr-pcvr-dx12`, `feat-xr-webxr` and `feat-xr-hand-tracking`.

`feat-sr-gamepads` follows from the input decision and blocks nothing.
