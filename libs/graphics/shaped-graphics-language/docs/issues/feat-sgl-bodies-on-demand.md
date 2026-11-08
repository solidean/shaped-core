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

## Measured, 2026-11-08

Release build, Windows, a Ryzen 9 9950X3D2, timing each outermost `checker::check_body_of` with a temporary counter.
A body's time includes the signatures it compiles on demand, so these are upper bounds.

- **The prelude:** of its 5.9 ms check, bodies are 2.9 ms, about half.
- **shaped-viewer's tracer pipeline**, `sgl describe` with its module directories: `check_modules` takes 15.9 ms and the program's own check 2.6 ms.
  Bodies are 18.5 ms of the process's checks, 4,270 of them, about 3 ms of that the prelude's.
  So bodies are some 85% of the modules' layer.

What this means for the design:

- A body a compile reaches is checked once per process with or without an artifact, since the first form has no impls.
  An artifact without impls saves only the interface's part and the bodies no program reaches.
- So how many of a layer's bodies a program reaches decides what this step and an impl-less module artifact are worth.
  Count that next: the distinct functions the tracer's entry points inline, against the 4,270 checked.
- If a program reaches most of its modules' bodies, serialized impls stop being "if it shows" and become what makes a module artifact pay.
  The prelude's artifact should pay either way: a program likely reaches few of its bodies, which is unmeasured, and its parse goes too.
