---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/binding/pipeline_layout.hh
  - symbol: sg::pipeline_layout_description::groups
---

# `pipeline_layout_description::groups` as a `cc::fixed_vector`

`groups` is a `cc::small_vector<binding_group_layout_handle, max_binding_groups>`, inline-capped so the common case never heap-allocates.
It can never exceed `max_binding_groups`, so `cc::fixed_vector` is the tighter match, and it has landed since the field was written.
The switch is a field type change; what needs checking is every caller that relies on `small_vector`-only API.
