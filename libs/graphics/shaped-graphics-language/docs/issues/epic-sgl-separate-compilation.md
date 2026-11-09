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

Decided with the maintainer on 2026-11-08, and written into the compilation model's [checked layers](../spec/incubator/compilation-model.md#separate-compilation-checked-layers):

- **Artifacts are cache entries, never shipped.** SGL is a development tool and what ships is compiled shaders, so an artifact is keyed by the compiler's build and has no format version.
- **Sections as wasm's:** imports, interface, contributions, impls, names, so a reader loads the interface without a body.
- **In memory, ids stay positions,** and an artifact is relocated once per load through its import table.
- **Artifacts carry their bodies checked,** and load whole.
- **No process keeps one for another, for now** (2026-11-09): a compile stays synchronous and does no I/O, and the gain was a fraction of a short process.
  Checking a body on demand has nowhere cheap to land under positional ids, as [feat-sgl-bodies-on-demand](feat-sgl-bodies-on-demand.md) found.
- **Early cutoff:** a dependent's key names its imports' interfaces, so a body edit re-checks no user.
- **Options are an overlay:** a layer is checked at the defaults, and a compile re-checks only what reaches an option it sets.

The steps, each a child issue when it is taken up, in this order:

1. **The prelude's artifact.** *Landed on 2026-11-08:* the 5.8 ms check is a 0.68 ms load; no process keeps it for another since 2026-11-09.
2. **The parse in the artifact.** *Tried on 2026-11-08 and not kept:* a parse is twenty times its source, and reading 9.6 MB back cost what parsing does.
   The compilation model's step 4 has the numbers; the per-process parse is [epic-sgl-prelude-check-cost](epic-sgl-prelude-check-cost.md)'s.
3. **Module artifacts.** *A first form landed on 2026-11-08:* one artifact per set of modules, the whole layer, so a 16 ms check is a 3.9 ms load; kept by no process, as the prelude's.
   What is left is in memory, since no process keeps a layer for another:
   imports and relocation, so a module is one layer shared by every set that reaches it, and early cutoff, so an edit re-checks only what it changed.
4. **Option overlays,** so a compile that sets options continues from its modules' layers too.
   *A first form landed on 2026-11-09:* a compile that sets only its program's own options does, which is every compile in the tree today.
   A module's own option still re-checks the whole set.
5. **Parallel layers,** and bodies on demand for readers that only check ([feat-sgl-bodies-on-demand](feat-sgl-bodies-on-demand.md)), if a measurement asks.
6. **The language server on artifacts.**

Out of scope here: cuts inside a single compile, the prelude parse's speed and the per-compile fixed cost, which are [epic-sgl-prelude-check-cost](epic-sgl-prelude-check-cost.md)'s.
