---
created: 2026-10-17
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_decl.cc
---

# A subgroup size a shader requires

`@preferred_subgroup_size` asks and promises nothing (CHK-371), which is what a tuning hint on code correct at any size means.
A shader correct at one size alone needs a separate spelling, behind a feature, which a device that cannot run that size refuses.
It lands once a shader needs one; turning the preference into a requirement instead would refuse shaders that work today.
