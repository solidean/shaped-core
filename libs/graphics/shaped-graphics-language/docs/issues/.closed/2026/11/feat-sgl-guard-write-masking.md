---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_guards.cc
closed: 2026-11-05
resolution: fixed
---

# Mask the writes of an invocation whose guard tripped before a barrier

Where a barrier, a derivative or a subgroup operation follows a tripped guard, the invocation goes on with a safe operand until after the last such statement (LEGAL-65).
Until then it writes buffers, images and workgroup memory with values computed from that operand, which another invocation may read.
Masking would test the flag at each store, atomic and image write between the guard and the end, and skip it.
Without masking the abort profile can still let a wrong value reach memory, which is what it exists to prevent.

## Done on 2026-11-05

LEGAL-67: once a guard may set the flag, every store to a buffer element or workgroup memory, and every atomic or image store standing as a statement, is skipped where the flag is set.
An atomic whose result a `let` binds, as in `let slot = counter.add(1)`, runs only where the flag is clear, and the `let` is 0 where it is set.
