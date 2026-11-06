---
created: 2026-11-05
priority: normal
tags: [xr, ci]
parent: epic-xr
---

# Run shaped-xr's tests against Monado headless in CI

Decided in `epic-xr`: try it, and accept not having it if GitHub runners cannot host it.

## Wanted

- Monado's headless compositor as the OpenXR runtime for `shaped-xr-test`, locally and in CI.
- A projection layer read back and checked, the way the headless swapchain is.

## Unknown

- Whether Monado runs headless on GitHub's Linux and Windows runners at all.
