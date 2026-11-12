---
created: 2026-11-05
priority: someday
tags: [webgpu, gi]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# Baked scenes: precomputed lighting a phone loads with the scene

A baked scene shows converged lighting on a device too slow to compute it live, for a scene and lights that do not change.
The Shelley pipeline already bakes path-traced lightmaps, unwrapping, denoising and compressing them, and is the first candidate producer.
Open: what sv loads (lightmaps, a probe volume, or both), and where the data lives.
A new file format is a decision for the user, not a migration to write; KTX2 beside the scene was the tentative answer.
