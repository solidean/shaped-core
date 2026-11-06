---
created: 2026-11-05
priority: someday
tags: [xr, wasm]
parent: epic-xr
---

# WebXR backend over the webgpu context

A second shaped-xr backend: WebXR's session and frame callback, and the WebGPU binding's projection layer, for `immersive-vr` and `immersive-ar`.
The frame loop inverts through the existing wasm main-loop hook.
WebGPU has no view instancing, so it renders a draw per view.

## Parked (2026-11-06)

Parked until the [WebXR/WebGPU binding](https://immersive-web.github.io/webxr-webgpu-binding/) ships: on 2026-11-06 it was an editor's draft, and no shipped browser was found.
sg has no webgl backend, so the shipped WebGL path is not open to us.
That search was not conclusive; Chrome's status page and Quest Browser's release notes are where to check.
The AR API stays compatible meanwhile: scene as a snapshot and hit tests as sources were chosen with WebXR in mind.
