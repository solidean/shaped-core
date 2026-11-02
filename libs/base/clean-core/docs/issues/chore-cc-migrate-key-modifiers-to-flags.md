---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/input.hh
  - symbol: sr::key_modifiers
  - symbol: cc::flags
---

# Migrate `sr::key_modifiers`, the last hand-rolled flag enum, to `cc::flags`

`sr::key_modifiers` is the last one still carrying its own operators plus a free `has_all`.
All of sg's have migrated — `buffer_usages`, `texture_usages`, `access_flags`, `pipeline_stage_flags`, `color_write_mask`, `accel_build_flags`.
