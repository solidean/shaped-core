---
created: 2026-10-17
parent: epic-sg-webgpu-gaps
tags: [webgpu]
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu
---

# webgpu cannot wait for a stream whose source has nothing ready

When a list touches a resource with such a stream in flight, it sees what landed so far, and a warning.
The other backends wait for the stream instead.
