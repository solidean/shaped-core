---
created: 2026-11-06
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md
  - file: libs/graphics/shaped-graphics-language/docs/issues/epic-sgl-modules-beyond-a-tracer.md
  - file: libs/graphics/shaped-graphics-language/docs/issues/epic-sgl-prelude-check-cost.md
---

# True separate compilation: a checked module is built once and shared

Today every process parses and checks the prelude, and every compile checks again each module it reaches.
slib pays a module's whole check per shader and per permutation, and every short-lived process pays the prelude's.
This is what matrix row D2 marks as "no separate compilation", and the part of F4's fixed cost no cut inside one compile removes.

The design is proposed in the compilation model's [checked layers](../spec/incubator/compilation-model.md#separate-compilation-checked-layers).
A checked module is an immutable layer, and a compile is a stack of layers with the program on top, so any number of compiles on any number of threads share one.

What the epic has to deliver, each a child issue when it is taken up:

- **Layers in memory.** Segmented flat tables and overlays for the four tables a program's check extends, so a compile stops copying the checked prelude and a module is checked once per process.
- **Ids across layers.** A layer's ids are stable without knowing what sits above it, which is the precondition for anything saved.
- **Saved layers.** A checked layer written to disk and loaded instead of checked, keyed by its sources, the compiler's version and its options.
  The invalidation is the cost the prelude-check epic shelved this on; many short-lived processes, or a prelude far past today's size, reopen it.
- **slib and the language server on layers.** slib builds each module's layer once per process for every shader and permutation; the language server keeps a module directory's layers live across edits.
- **Interface-only layers, if ever.** A cheaper layer of signatures alone is an optimization of building one, never a different thing crossing between modules, since inlining and per-use generics need the bodies.

Out of scope here: cuts inside a single compile, the prelude parse's speed and the per-compile fixed cost, which are [epic-sgl-prelude-check-cost](epic-sgl-prelude-check-cost.md)'s.
