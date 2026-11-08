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
- **Bodies are checked from source on demand** in the first form; serialized impls come only if that shows.
- **Early cutoff:** a dependent's key names its imports' interfaces, so a body edit re-checks no user.
- **Options are an overlay:** an artifact is checked at the defaults, and a compile re-checks only what reaches an option it sets.

The steps, each a child issue when it is taken up, in this order:

1. **Bodies on demand** ([feat-sgl-bodies-on-demand](feat-sgl-bodies-on-demand.md)): a layer checks a body when a compile first needs it.
2. **The prelude's artifact:** interface and contributions, no imports, bodies from source.
3. **Module artifacts:** imports, relocation on load, early-cutoff keys, kept by slib's persistent cache.
4. **Option overlays,** so a compile that sets options continues from the artifacts too.
5. **Parallel layers,** and serialized impls if checking bodies from source shows.
6. **The language server on artifacts.**

Out of scope here: cuts inside a single compile, the prelude parse's speed and the per-compile fixed cost, which are [epic-sgl-prelude-check-cost](epic-sgl-prelude-check-cost.md)'s.
