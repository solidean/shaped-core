---
created: 2026-10-18
refs:
  - symbol: sv::impl::is_similarity
---

# `is_similarity` belongs in typed-geometry

Whether a linear map is a rotation, possibly mirrored, times one uniform scale is a question about `tg::mat3`, not about emitters.
sv asks it to tell where an emitter's world area is exact; typed-geometry classifying a linear map would replace it.
