---
created: 2026-11-08
parent: epic-sgl-separate-compilation
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_modules.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md
---

# A layer checks a function's body when a compile first needs it

Step 3 of the compilation model's [checked layers](../spec/incubator/compilation-model.md#separate-compilation-checked-layers), and step 1 of [the epic](epic-sgl-separate-compilation.md).
Today `check_modules` and `check_prelude` check every body of every file they hold, whether or not a program reaches it.
Loading an artifact without impls stands on this: a body is checked from source the first time a compile inlines it, instantiates it or evaluates it.

What has to hold:

- A body checked on demand gives the same result as one checked with its module: the same ids it adds, the same flat trees.
- A layer stays immutable for every reader, so a body checked on demand lands in a part of the layer behind a lock, or in the compile's own layer.
  Which of the two is the design question here.
- A compile that only checks the program reaches no module body it does not evaluate or instantiate.
- A module's own check still checks every body: its tests, the language server on its file, and an artifact's making.

Measure first: how much of a layer's check is bodies, for the prelude and for shaped-viewer's modules, through the `sgl profile` tests.
If bodies are a small share, this step's own win is small, and it is still the precondition for artifacts.
