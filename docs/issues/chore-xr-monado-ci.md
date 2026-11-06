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

## Status (2026-11-06)

It works locally on Windows, built from Monado 2b7f5cb: `tests/session-test.cc` runs real sessions on vulkan and dx12, and `xr-cube` runs on both.
[testing.md](../../libs/graphics/shaped-xr/docs/testing.md) is how, and what Monado does not prove.
Without Monado selected those tests skip, so the default suite and CI still run none of them.

## Left

- A way to get Monado that is not a hand-run build: `dev.py install monado` at a pinned commit, or a package on Linux.
- dev.py or the suite selecting it, so the tests run without three environment variables by hand.
- Whether Monado runs headless on GitHub's Linux and Windows runners at all, which also need a Vulkan device, likely lavapipe.
- Reading a projection layer back on vulkan: the runtime's images have no `copy_src`, so only dx12 checks pixels today.
