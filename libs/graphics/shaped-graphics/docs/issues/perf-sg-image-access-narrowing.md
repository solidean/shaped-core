---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/barrier/access_inference.hh
  - symbol: sg::shader_access_of
---

# An image binding counts as written whatever its `access` says

`shader_access_of` infers a bound view's access from its view class: readwrite buffers and images write.
An image whose binding declares a read-only `access` still counts as written, and so costs the barrier a write implies.
Narrowing it means reading the binding's `access` for images too.
