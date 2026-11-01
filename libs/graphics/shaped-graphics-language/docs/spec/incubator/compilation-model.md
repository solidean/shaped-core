# The Compilation Model

*Incubator: not normative.*

## The idea

**SGL compiles to shader text, and the text's own compiler optimizes it.**

The mid-term targets are HLSL for dx12, HLSL for vulkan, WGSL, MSL, and maybe GLSL.
The two HLSL targets are separate outputs with their addresses already resolved: SGL does not emit a portable HLSL that needs another pass.

**So SGL is not an optimizing compiler, and will not be one for a long time.**
For now it optimizes nothing.
Mid-term it folds easy constants and eliminates dead code and unused variables.
Unused-variable elimination is the one with a reason of its own: hazard tracking is only as precise as the set of bindings a function really touches.

**A module is the unit of compilation, and an interface is what crosses between modules.**

* Each file is parsed to its AST on its own.
* All files of one module compile together, because they name each other unqualified.
* Anything from a foreign module is reached through a qualified name only.
* Each module produces an **interface**: its names, types and function signatures.
  Ideally a cheap pass produces it, without real compilation.
* A module then depends on its own files and on the interfaces of the modules it uses, so all modules compile in parallel.

The model is close to how WebAssembly modules relate.
"Compile" here stops just beyond type checking: no function is inlined yet.
The line between the interface pass and compilation may blur, because a type may need a function evaluated.
That is acceptable as long as a large shader library can be preprocessed ahead of its users.

**Within a module, name resolution, type checking and evaluation are one pass.**
They cannot be separated: overloading is by type, so even an unqualified name needs types to resolve, and a type expression may need evaluation.

* Compilation goes symbol by symbol: functions, bindings, types.
* A symbol being compiled is marked **in compilation**.
* Resolving a name may need another symbol fully checked, and that symbol is then compiled on demand.
* On-demand is either recursion, or the pass is asynchronous and awaits the other symbol, which lets one module go wide internally.
* Reaching a symbol that is already in compilation is a dependency cycle, and it is an error that names the loop.

**Inlining happens inside that pass, and it is mandatory.**
Lambdas, nested functions that reference the variables around them, and delegating bindings all need it before valid target text exists.
It cannot be a separate later pass either: type parameters and compile-time arguments are duck typed, as C++ templates are, and only really checked once instantiated.
So an emitted entry point is one flat function ([function-model.md](function-model.md)).

**A binding list is never passed.**
A binding is either a global of the target, or, for a delegating binding, a local function that is inlined.

**The emitted text is meant to be read.**
A shader debugger or a capture tool shows the target text, so names, structure and semantics stay recognizable there.
Somebody outside the SGL ecosystem should not find it opaque.
A minify mode may come later, and it is a mode.
Every name an emitter writes goes through one minting helper that takes the desired name and returns one that is free.
So names can be chosen for the reader, and a collision is impossible by construction.
A name that is reserved in one target gets a trailing underscore in that target only.
An entry point keeps its name, since the host asks for it, and one that collides in any target is an SGL error.

**The stages compile apart.**
A vertex and a pixel entry point share only the type they pass, and nothing in one stage's text depends on the other.
On Metal that means bindings take the first buffer indices and vertex buffers come after them.

**SGL follows the rules of `sg` and `tg`** unless there are very good reasons against it, with as few gotchas as is reasonable.
Matrix layout is the first case: column-major, the vector on the right, and a `tg::mat4f` uploads as its bytes.
A very good reason looks like the transformation matrix of a ray tracing instance, whose layout and meaning the APIs give.

**The prelude is a file on disk**, written in SGL, that the compiler reads and places in front of the program.
That needs no multi-file compilation.

## Separate compilation: checked layers

*Proposed, for review; nothing here is built yet.*

### What it costs today

Measured on 2026-11-01, release build, Windows, a Ryzen 9 5900X:

| | cost |
|---|---|
| a program's own check behind an already-checked prelude, in-process | 0.3 ms empty, 0.6 ms for a small program |
| of that, copying the checked prelude's state (perf-sgl-compile-copies-checked-prelude) | about two thirds, 1.9 of 2.9 ms in a debug build |
| parsing and checking the prelude, once per process | about 30 ms |
| a module, in every compile that reaches it | its whole check, every time (epic-sgl-modules-beyond-a-tracer) |
| a trivial `sgl emit`, all of it but the OS's process start | about 47 ms |

So a compile in a long-lived process is already cheap, and what is not is the work no program owns.
The prelude is checked again by every process: the CLI per file, slib per application, every test binary.
A module is checked again by every compile that reaches it, which slib pays per shader and per permutation.
And a compile copies the whole checked prelude to continue from it, because the checker writes to the state it continues.

### The idea

**A checked module is an immutable layer, and a compile is a stack of them with the program on top.**

* The prelude is the bottom layer.
* Each module is a layer over the layers of the modules it uses, and over the prelude.
* The program's check writes only to its own layer, and reads the ones below it.
* A layer, once checked, is never written again, so any number of compiles on any number of threads share it.

This answers "what an interface holds" from the list of open questions: everything the layer's check produced.
Inlining is mandatory and a generic is checked where it is instantiated, so a user needs a module's checked bodies, not only its signatures.
A cheap interface-only pass stays possible later, as an optimization of building a layer, not as a different thing crossing between modules.

### What a layer is

The checker's state is flat tables indexed by ids, `checked_module::symbols`, `types`, `functions` and the rest, plus side tables of its own.
A layer is a contiguous part of each flat table, and the side tables split the same way.

* **A flat table becomes segmented.**
  A layer owns its segment, and a lookup by id reaches the segment of the layer the id belongs to.
* **A scope or a cache a program extends gets an overlay.**
  The program's layer holds what it adds, and a lookup reads its own overlay, then the layer below.
  That covers the module and type scopes, the operator sets, and the interning of generic instances: `interned_types`, where a program's `buffer[foo]` instantiates a prelude template.
* **What a program's check may not do is write into a lower layer.**
  Today nothing stops it, which is why a compile copies the prelude rather than sharing it.
  The first step therefore finds every such write and turns it into an overlay entry, and a test holds the lower layer unchanged across the corpus.

### Ids

The decision the rest follows from: **an id names its layer.**

A module's layer has to be the same bytes whichever program uses it, and whichever other modules sit beside it in that compile.
An id that is a global index cannot do that: the base of `view`'s ids would move with the modules checked before it.
So an id carries a layer number and an index within the layer, and the layer number is the process's, assigned when the layer is made or loaded.
A lookup is then a load of the layer's segment and an index into it, with no search.

Two alternatives were considered:

* **Rebase on use:** give a module's layer a base per compile and rewrite its ids.
  That is a pass over the whole layer per compile, which is the copy this design removes.
* **One global order of all modules:** possible in a closed world, and slib's module directories are not one.

### Saving a layer

A layer can be written as bytes and read back: a **checked-module artifact**.

* It is keyed by its sources, the keys of the layers below it, and the compiler's own build, so a different compiler never reads it.
* It is a cache, never a file anyone keeps: a miss checks the module again, and an artifact that fails to read is a miss.
  So it has no format version and no migration.
* Loading it maps its dependencies' layer numbers to this process's, the one rewrite it needs, once per process rather than per compile.

Two places keep one:

* **The prelude's**, so a process starts without checking it.
  First in the persistent cache slib already uses, which needs no build step; later perhaps embedded, which needs the compiler to run at build time to produce the bytes it then links in.
* **A module's**, in the same persistent cache, so slib checks a module once per machine rather than once per process.

### What it buys

* A compile behind the prelude stops copying it: about two thirds of a small program's in-process compile.
* A module is checked once per process, then once per machine with artifacts.
* A process stops checking the prelude: the ~30 ms that every CLI call, application and test binary pays.
* Modules with no dependency between them can be checked in parallel, since each is its own layer.

### Steps

Each step stands on its own and is measured by the `sgl profile` tests.

1. **The prelude as a shared layer.**
   Segmented tables and overlays for the prelude and the program, ids that name their layer, and no write into the prelude's layer.
   The prelude is still checked once per process; what goes is the copy.
2. **A module is a layer**, checked once per process and kept, reused by every compile that reaches it.
3. **Artifacts**: saving and loading a layer, first for the prelude, then for modules, through slib's persistent cache.
4. **Parallel layers**, and then perhaps an interface-only pass.

### Open in this design

* How wide an id is: 8 bits of layer and 24 of index fit today's 32, and limit a process to 256 layers alive at once.
  A process that checks hundreds of modules would need wider ids, or layers that are dropped and their numbers reused.
* Whether a segmented lookup's extra load shows in the emitters, which read the checked module more than the checker writes it.
* Which of the checker's writes into the prelude's state today are real dependencies on the program, rather than caches that belong in an overlay.
* Whether the language server, which keeps its own parse of the prelude, moves onto the same layer.

## What it touches

* The toolchain: module discovery, the interface format, and the parallel driver.
* Every semantic phase: they are one demand-driven pass per module, not a pipeline of passes.
* The function model ([function-model.md](function-model.md)): no recursion is what makes a cycle an error rather than a fixpoint.
* Types as values ([types-as-values.md](types-as-values.md)): evaluation inside type checking is where the interface pass and compilation blur.
* Modules ([modules-and-prelude.md](modules-and-prelude.md)): several files declare one module, and foreign names are always qualified.
* Binding effects ([binding-effects.md](binding-effects.md)): unused-variable elimination feeds the set of bindings a function uses.
* The emitters: one per target text format, reading the same checked program.

## Already fixed by the syntax

* The AST pass is per file and looks no names up, so files parse in parallel and in any order.
* Names in the AST are spans, so nothing is resolved before the module is known.
* `.` is the only accessor, so a qualified foreign name needs no syntax of its own.

## Open

* What an interface holds when a signature's type needs a function body evaluated.
  Proposed: the whole checked layer, bodies included (see "Separate compilation: checked layers").
* Whether an interface is a file on disk, and whether it is SGL text.
  Proposed: a cache entry of the checked layer's bytes, never a kept file and never SGL text.
* Whether on-demand compilation is recursive or asynchronous.
* How a dependency cycle is reported when its loop passes through a type expression.
* How the files of a module are found beyond a list of directories.
  What is settled: the compiler takes a library of in-memory sources and never reads a file, and groups them by their `module` line (CHK-346).
  Finding them is the caller's: the `sgl` tool's `--module-dir`, slib's module directories, the language server's setting.
* How an inlined early `return` is written in targets that have no `goto`.
* How much of an inlined program's source structure the readable text can keep.
