---
created: 2026-11-05
tags: [webgpu, raster, gi]
parent: epic-sv-raster-bound
blocked-by: [feat-sg-polyfill-throughput-spike, perf-sg-polyfill-unsorted-tree, feat-sv-raster-bound-skeleton]
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# `raster_bound`: probe GI traced through the ray-query polyfill (C2)

A probe volume in the style of DDGI, written from the paper, with its ray budget set by S0's measurements ([raster-bound.md](../plans/raster-bound.md#probe-gi)).
A probe's hit is lit by the previous frame's probes and reads the secondary material; on dx12 and vulkan the same probes trace on hardware.
Probe relocation and classification answer probes inside walls.
Whether phones update live or load a bake is decided from S0.
Sample: a CAD assembly in a closed room, lit indirectly, against the path tracer.
