---
created: 2026-11-06
priority: next
tags: [xr, ar]
parent: epic-xr
---

# What does Quest Link really offer for AR?

No page says whether environment depth, scene, anchors or raycasts reach Quest Link; Unity's docs only call "spatial data" experimental there.
This decides the order of the AR work, and it needs the headset.

## To do

Run this over Link on a machine with the headset connected and awake, with Developer Runtime Features on and Quest Link restarted after:

```bash
uv run dev.py example xr-probe
```

Paste its output here.
`features` is what the system answers, so a listed extension with `no` beside it does not count.
Plane tracking through `XR_EXT_spatial_*` is the second thing to read off, since that decides when `feat-xr-scene` can drop `XR_FB`.

## What it decides

Decided on 2026-11-06: if depth or scene does not reach Link, `feat-android-apk` moves ahead of the AR work, rather than building it blind.
Even a `yes` is only a listing, and `feat-xr-environment-depth` is what proves the data arrives.
