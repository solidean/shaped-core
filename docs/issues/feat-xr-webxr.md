---
created: 2026-11-05
priority: normal
tags: [xr, wasm]
parent: epic-xr
---

# WebXR backend over the webgpu context

A second shaped-xr backend: WebXR's session and frame callback, and the WebGPU binding's projection layer, for `immersive-vr` and `immersive-ar`.
The frame loop inverts through the existing wasm main-loop hook.
WebGPU has no view instancing, so it renders a draw per view.
