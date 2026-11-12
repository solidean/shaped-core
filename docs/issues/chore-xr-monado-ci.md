---
created: 2026-11-05
priority: normal
tags: [xr, ci]
parent: epic-xr
claimed-by: Claude (for Philip)
claimed-at: 2026-11-06T11:00Z
---

# Run shaped-xr's tests against Monado headless in CI

Decided in `epic-xr`: try it, and accept not having it if GitHub runners cannot host it.

## Wanted

- Monado's headless compositor as the OpenXR runtime for `shaped-xr-test`, locally and in CI.
- A projection layer read back and checked, the way the headless swapchain is.

## Status (2026-11-06)

It works locally on Windows, built from Monado 2b7f5cb: `tests/session-test.cc` runs real sessions on vulkan and dx12, and `xr-cube` runs on both.
[testing.md](../../libs/graphics/shaped-xr/docs/testing.md) is how, and what Monado does not prove.
`dev.py install monado` builds it, and `dev.py test` then selects it, on Windows.
Without it those tests skip, so CI still runs none of them.

## Left

- Monado on Linux: `dev.py install monado` refuses there, and a distro package or a build would do.
- Whether Monado runs headless on GitHub's Linux and Windows runners at all, which also need a Vulkan device, likely lavapipe.
