---
created: 2026-11-05
priority: normal
tags: [xr]
parent: epic-xr
---

# PCVR on dx12: the `XR_KHR_D3D12_enable` binding

The dx12 binding exists: `sxr::graphics_api::dx12`, run by `xr-cube --dx12`, with sg rendering into its own images and copying (see shaped-xr's design).

## Wanted

- A mirror view of the headset in an `sr::window`.
- SteamVR verified; only Quest Link has run so far.
- A refusal, or a fallback to a pass a layer, where dx12's view instancing tier is too low; `xr-cube` only checks the feature.
