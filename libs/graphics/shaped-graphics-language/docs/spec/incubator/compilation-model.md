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

*Decided with the maintainer on 2026-11-08; steps 1 and 2 are built in a first form, the rest is not.*

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

A user of a module needs its interface always, and a body only where it inlines it, instantiates it or evaluates it.
So a saved layer holds the interface and the bodies apart, and a reader that needs no body reads none ("Artifacts" below).

### What a layer is

The checker's state is flat tables indexed by ids, `checked_module::symbols`, `types`, `functions` and the rest, plus side tables of its own.
A layer is a contiguous part of each flat table, and the side tables split the same way.

* **A flat table becomes segmented.**
  A layer owns its segment, and a lookup by id reaches the segment of the layer the id belongs to.
* **A scope or a cache a program extends gets an overlay.**
  The program's layer holds what it adds, and a lookup reads its own overlay, then the layer below.
  That covers the module and type scopes, the operator sets, and the interning of generic instances: `interned_types`, where a program's `buffer[foo]` instantiates a prelude template.
* **What a program's check may not do is write into a lower layer.**
  It already does not, into `checked_module`'s tables: a program's check only appends to them.
  `checked-prelude-test.cc` holds that across the corpus, the samples, sg's shaders and the programs written to report.
  What it does extend is four of the checker's own tables: the merged module scopes, the operator sets, the type scopes an extension adds to, and the interned instances.
  Those are the overlays, and the rest of the checker's lists are appended to like `checked_module`'s.

### Ids

**In memory, an id stays a position in its whole table, and an artifact is relocated once, when it is loaded.**

An artifact cannot hold such positions: the base of `view`'s ids moves with the modules checked before it.
So an artifact names its own entries by its own index spaces, and names another layer's through an import, as a wasm module does.
Loading appends its tables behind the layers it stands on, adds that base to each of its own ids, and resolves each import to the id it names there.
That is one pass over the artifact per stack of layers a process builds, never one per compile.

Considered and set aside:

* **An id that names its layer**, 8 bits of layer and 24 of index.
  One module's layer would be shared in memory by every stack that holds it, for an extra load per lookup and at most 256 layers alive.
  It comes back if the per-stack relocation shows in a measurement.
* **Rebase on use:** a pass over the whole layer per compile, which is the copy step 1 removed.
* **One global order of all modules:** possible in a closed world, and slib's module directories are not one.

### Artifacts

A checked layer can be written as bytes and read back: a **module artifact**.

**An artifact is a cache entry, never a file anyone ships or keeps.**
SGL is a development tool, and what ships is compiled shaders.
A process that compiles at run time ships the compiler too, so the artifacts it needs are that compiler's.

* It is keyed by its module's sources, the interface keys of the artifacts it imports, and the compiler's own build, so a different compiler never reads it.
* So it has no format version and no migration, and its layout follows the checker's tables.
* A miss, a corrupt artifact and a truncated one all check the module again from source: deleting every artifact never changes a result.
* Only a module whose check reported nothing has one; a module that reports is checked from source by every compile, which reports it.
* The compiler writes and reads bytes, and where they are kept is its host's: slib's persistent cache, a table a frozen build embeds, or nowhere.
  sgl depends on clean-core alone, so the store is never sgl's.

**No process keeps an artifact for another, for now.** *Decided with the maintainer on 2026-11-09, after one was kept for a day.*
A compile reads nothing but its arguments, which is what makes its result cacheable, and it is synchronous.
A store behind it is file I/O inside the compile, and done properly that is async end to end, through the compile itself.
What it bought did not pay for that: about 30 ms of a 120 ms `sgl describe`, in a process whose start dominates anyway.
Live editing keeps its layers in memory for the whole process, and a batch mode would pay the prelude once for many sources.
The format stays, held by its round-trip tests, for a host that one day keeps artifacts through an async seam.

**Its sections follow wasm's, so a reader jumps to the one it needs.**

* **Imports:** each declaration of another layer it names, by module, name, kind and a hash of its signature.
* **Interface:** everything a user of the module can name, checked.
  Symbols, types, struct fields, enum cases with their values, functions' signatures and attributes, parameters, bindings, pipelines, samplers, and each `const` as its value.
  A value a type position needed is stored computed, so a user reads it without the body that computed it.
* **Contributions:** what the module adds to scopes it does not own, extensions, operators and interned instances, merged on load.
* **Impls:** the checked tables of the bodies, one entry per function behind a table of offsets.
* **Names:** files and spans for the language server, which emit never reads.

**An artifact carries its bodies checked, and loads whole: a body is copied, never checked again.**
Checking a body on demand was the first plan, and it has nowhere cheap to land under positional ids ([feat-sgl-bodies-on-demand](../../issues/feat-sgl-bodies-on-demand.md)).
A body checked into the shared layer would grow it under a program's ids, and could intern a type the program holds already under another id.
A body checked into the compile's own layer costs every compile the bodies it reaches, which a layer checked whole charges nobody.
So a reader that needs no body skips the impls by their offsets, and one that does copies them with the rest.
The files are parsed from source, since the flat trees read the AST: a parse read back costs what parsing costs (step 4 below).

**Early cutoff: a dependent's key names its imports' interfaces, not their sources.**

* An edit to a body that leaves its module's interface alone makes no artifact of a module using it stale.
* A dependent whose own check read a body of an import, a generic it instantiated or a function it evaluated, names that body's hash in its key too.

**Options are an overlay.**

* An artifact is its module checked at its options' defaults.
* Each declaration records the options it reaches (CHK-355), through other modules' declarations too.
* A compile that sets options checks again, from source and in its own layer, exactly the declarations that reach an option it sets; those shadow the artifact's.
* So every permutation slib compiles shares one artifact per module, and each pays only for what its options change.

Two artifacts pay for themselves first:

* **The prelude's**, so a process starts without parsing or checking it.
* **A module's**, so slib checks a module once per machine rather than once per process.

### What it buys

* A compile behind the prelude stops copying it: about two thirds of a small program's in-process compile.
* A module is checked once per process, then once per machine with artifacts.
* An edit to a module's body re-checks no module that uses it.
* A compile that sets options continues from the artifacts, and checks again only what its options reach.
* A process stops checking the prelude: the ~30 ms that every CLI call, application and test binary pays.
* Modules with no dependency between them can be checked in parallel, since each is its own layer.

### Steps

Each step stands on its own and is measured by the `sgl profile` tests.

1. **The prelude as a shared layer.** *Landed on 2026-11-01: a small program's check behind the prelude went from 0.61 to 0.10 ms.*
   Segmented tables and overlays for the prelude and the program.
   It needs no change to ids: with two layers, the program's ids continue after the prelude's as they do today.
   The prelude is still checked once per process; what goes is the copy.
2. **A module is a layer**, checked once per process and kept, reused by every compile that reaches it.
   This is where ids start to name their layer, since a module's layer then sits at different heights in different compiles.
   *A first form landed on 2026-11-04: the modules a program reaches are one layer over the prelude's, `check::check_modules`, with ids unchanged.*
   The driver keeps one per set of modules, beside the library it parsed once for the process, for compiles that set no options.
   A shaped-viewer hit group's `describe` went from 81 to 54 ms, and its check from 71 to 23 ms.
   What it does not do is share a module between two sets that both reach it, which is what ids naming their layer would buy.
3. **The prelude's artifact.** *Landed on 2026-11-08: the prelude's 5.8 ms check is a 0.68 ms load of 1.6 MB, in a release build.*
   `check::save_artifact` and `load_artifact` write and read the checker's whole state after the prelude, bodies included, under a host's key.
   In a fresh `sgl` process the load was 2.3 ms with a file's read, and 8 ms from blob-cache with its open; the prelude's parse, about 13 ms there, is what a process still pays.
   No process keeps it since 2026-11-09 (see [artifacts](#artifacts)).
4. **The parse in the artifact.** *Tried on 2026-11-08 and not kept: reading a parse back costs what parsing it does.*
   A parse is some twenty times its source: the prelude's files took the artifact from 1.6 MB to 9.6 MB.
   In a fresh `sgl` process that file's read was 5.0 ms and its walk 3.6 ms, against about 1 ms to parse the two files beside `builtins.sgl`.
   `builtins.sgl` is most of the prelude, and the builtin registry parses it as it is built, 8.6 ms there, before any store is asked.
   What would cut the per-process parse is a faster parse or a smaller `builtins.sgl` ([epic-sgl-prelude-check-cost](../../issues/epic-sgl-prelude-check-cost.md)), not an artifact.
5. **Module artifacts.** *A first form landed on 2026-11-08: the layer of one set of modules, the prelude's part included, as one artifact.*
   Within one build the same modules in the same order make the same ids, so it needs neither imports nor relocation.
   `check::load_modules_artifact` plans the program's modules as `check_modules` does and reads the layer back for them.
   A module's flat test trees are in it too.
   shaped-viewer's tracer pipeline: its modules' layer is a 3.9 ms load against a 16 ms check, and a whole `sgl describe` fell from about 120 to 90 ms.
   No process keeps it since 2026-11-09 (see [artifacts](#artifacts)).
   What it does not do is share one module between two sets, or cut a dependent off from an edit of a body: imports and relocation are those.
6. **Option overlays**, so a compile that sets options continues from the artifacts too.
7. **Parallel layers**, and bodies on demand for a reader that only checks, if a measurement asks for them.

### Open in this design

* Whether the per-stack relocation shows, which would bring back ids that name their layer.
* How a declaration records an option it reaches through another module's declaration, which the overlay needs across artifacts.
* Whether the language server, which keeps its own parse of the prelude, moves onto artifacts.

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

* Whether on-demand compilation is recursive or asynchronous.
* How a dependency cycle is reported when its loop passes through a type expression.
* How the files of a module are found beyond a list of directories.
  What is settled: the compiler takes a library of in-memory sources and never reads a file, and groups them by their `module` line (CHK-346).
  Finding them is the caller's: the `sgl` tool's `--module-dir`, slib's module directories, the language server's setting.
* How an inlined early `return` is written in targets that have no `goto`.
* How much of an inlined program's source structure the readable text can keep.
