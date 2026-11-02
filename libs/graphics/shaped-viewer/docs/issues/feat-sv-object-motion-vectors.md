---
created: 2026-10-17
parent: epic-sv-denoising-integration
blocked-by: [feat-sv-mesh-placement-identity]
refs:
  - file: libs/graphics/shaped-viewer/tests/motion-vector-test.cc
---

# Object motion vectors for the temporal denoiser

Object motion vectors need scene items with an identity that survives a frame, which they do not have.
Camera motion covers a static scene.
