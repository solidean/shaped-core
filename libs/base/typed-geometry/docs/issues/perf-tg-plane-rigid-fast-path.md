---
created: 2026-10-17
priority: someday
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/transform/homogeneous_transform.hh
  - symbol: tg::plane
---

# Faster rigid / similarity paths for transforming a `plane`

The plane registration goes through the cofactor matrix for every class.
That is correct everywhere and exact for a rigid transform, but a rigid one could just rotate the normal.
Add the fast path if it ever shows up in a profile.
