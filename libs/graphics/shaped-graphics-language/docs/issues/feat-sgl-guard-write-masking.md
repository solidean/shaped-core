---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_guards.cc
---

# Mask the writes of an invocation whose guard tripped before a barrier

Where a barrier, a derivative or a subgroup operation follows a tripped guard, the invocation goes on with a safe operand until after the last such statement (LEGAL-65).
Until then it writes buffers, images and workgroup memory with values computed from that operand, which another invocation may read.
Masking would test the flag at each store, atomic and image write between the guard and the end, and skip it.
Without masking the abort profile can still let a wrong value reach memory, which is what it exists to prevent.
