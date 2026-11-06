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

- **`libs/graphics/shaped-xr`**: system, session and frames on vulkan and dx12, OpenXR's actions, and `XR_FB_passthrough`.
  Its [design](../../libs/graphics/shaped-xr/docs/design.md) is the detail.
- **`xr-cube`** ran in a Quest 3 over Quest Link: one SGL source, both eyes in one view-instanced pass, the room behind it, and the cube grabbed with a controller.
- **The sg hooks are done on dx12 and vulkan**: external device requirements, wrapped native textures, view instancing and SGL's `@view_index`.
- The NDK presets build, but nothing packages an APK, so nothing runs on a Quest by itself.

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

## Settled by building it

- **Headset images are wrapped sg textures**, not an `sg::swapchain`: the frame is wait / begin / end, not acquire / present.
- **View instancing is phase one**, because on Adreno multiview is the only fast stereo.

## Still to settle

- The session API for environment depth, scene, anchors and hit tests: `question-xr-ar-capability-api`.

The design deck, its answers and annotations are in `.tmp/xr-design/` for now; what is decided here moves into shaped-xr's docs when the library starts.

## Phases

Done: the sg hooks, shaped-xr's core, `xr-cube` in the headset, actions, passthrough and the dx12 binding.

1. The AR capability API, designed first: `question-xr-ar-capability-api`.
2. Tests without a headset: `chore-xr-monado-ci`.
3. The rest of what started: `feat-xr-controller-actions` (drawing the controllers), `feat-xr-passthrough` (blend modes) and `feat-xr-pcvr-dx12` (mirror window, SteamVR).
4. Standalone on the Quest: `feat-android-apk`.
5. `feat-xr-webxr` and `feat-xr-hand-tracking`.

`feat-sr-gamepads` follows from the input decision and blocks nothing.
