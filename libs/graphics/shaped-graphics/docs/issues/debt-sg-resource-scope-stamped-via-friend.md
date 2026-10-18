---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/resource/raw_buffer.hh
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/resource/raw_texture.hh
  - symbol: sg::raw_buffer::scope
  - symbol: sg::raw_texture::scope
---

# A resource's lifetime scope is stamped after construction, through a friend

`raw_buffer::scope()` / `raw_texture::scope()` read a `mutable` field that `context_transient_scope` sets on the handle the backend just returned.
Every backend already receives `allocation_info::scope` when it creates a resource, so each could forward it to the `raw_buffer` / `raw_texture` constructor instead.
The field then becomes `const` and the friend goes away.

It touches all eight backend resource classes and the four test fakes that construct the bases, metal's included, for no behavioural change.
So it is its own change rather than part of the one that added the stamp.
