---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/threading.md
---

# Free-threaded resource creation

Asyncs, layouts and samplers are free-threaded on every backend, and resource creation is still bound.
Nothing forces that: a `main_thread` or `single_threaded` backend could record a buffer's or texture's description and make the object on first use, as webgpu's layouts do.
An initial upload would then have to queue too.
What it buys is concurrent scene loading against a thread-bound context without funnelling every create through its thread.
See [concepts/threading.md](../concepts/threading.md).
