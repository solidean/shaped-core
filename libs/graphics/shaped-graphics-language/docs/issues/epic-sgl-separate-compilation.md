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
- **Keeping artifacts is a host's capability, off by default** (2026-11-09): a compile stays synchronous and does no I/O.
  A host that keeps them fetches them asynchronously before the compile and hands the loaded layers in.
  Checking a body on demand has nowhere cheap to land under positional ids, as [feat-sgl-bodies-on-demand](feat-sgl-bodies-on-demand.md) found.
- **Early cutoff:** a dependent's key names its imports' interfaces, so a body edit re-checks no user.
- **Options are a second layer** (2026-11-09): an option never changes the names a module declares, so a module's layer is checked once at the defaults.
  Whatever reaches an option, types included, is a second layer a compile that sets one checks again over the first.

The steps, each a child issue when it is taken up, in this order:

1. **The prelude's artifact.** *Landed on 2026-11-08:* the 5.8 ms check is a 0.68 ms load; no process keeps it for another since 2026-11-09.
2. **The parse in the artifact.** *Tried on 2026-11-08 and not kept:* a parse is twenty times its source, and reading 9.6 MB back cost what parsing does.
   The compilation model's step 4 has the numbers; the per-process parse is [epic-sgl-prelude-check-cost](epic-sgl-prelude-check-cost.md)'s.
3. **Module artifacts.** *A first form landed on 2026-11-08:* one artifact per set of modules, the whole layer, so a 16 ms check is a 3.9 ms load; kept by no process, as the prelude's.
   *Since 2026-11-09 the library is one layer* (`check::check_library`), so a module is checked once per process whatever reaches it: shaped-viewer's 139 ms of sets is 27 ms.
4. **A chain of module layers:** the library's modules in one order, dependencies first, each a layer over the one before; an edit re-checks from its module on.
   *Landed on 2026-11-09:* an edit of shaped-viewer's last module checks 1.4 ms of its 24.7 ms chain, of its first all of it.
   Each link copies the tables below it ([perf-sgl-layer-freeze-copies](perf-sgl-layer-freeze-copies.md)).
   The order follows the edit: a module being edited goes as late as the modules using it allow, 8.6 ms an edit on average there.
5. **Early cutoff, through relocation:** an edit that leaves a module's interface alone relocates the layers after it rather than checking them again.
   *Landed on 2026-11-09:* `check::relocate_link`, held byte for byte to a fresh check by a test; past an edit of shaped-viewer's `scene`, the 9 links after it move in 3.7 ms.
   A changed interface still checks every link naming anything of its module, the part of it that did not change included.
6. **Options as a second layer:** a compile that sets a module's option checks only what reaches it.
   *A first form landed on 2026-11-09:* a compile that sets only its program's own options continues from the layers.
   *A second, the same day:* a link is checked with its module's values, so a compile that sets one checks that module's link and those after it, and shares the rest.
   The declaration's grain waits for relocation (step 5).
7. **Per-module artifacts, kept where a host asks,** relocated on load.
8. **The language server on layers.** *Landed on 2026-11-09:* an analysis continues from the layers every compile shares.
   A keystroke in a shaped-viewer program is 3.6 ms; in a module it uses 6.1 to 13.3 ms by its place in the chain.
9. **Parallel layers,** and bodies on demand for readers that only check ([feat-sgl-bodies-on-demand](feat-sgl-bodies-on-demand.md)), if a measurement asks.

Out of scope here: cuts inside a single compile, the prelude parse's speed and the per-compile fixed cost, which are [epic-sgl-prelude-check-cost](epic-sgl-prelude-check-cost.md)'s.
