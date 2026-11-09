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

*Decided with the maintainer on 2026-11-08, and its remaining steps on 2026-11-09; steps 1 to 9 are built in a first form, the rest is not.*

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
  Within one library it is one, and that is what a chain of module layers stands on before relocation exists (step 6 below).

### Artifacts

A checked layer can be written as bytes and read back: a **module artifact**.

**An artifact is a cache entry, never a file anyone ships or keeps.**
SGL is a development tool, and what ships is compiled shaders.
A process that compiles at run time ships the compiler too, so the artifacts it needs are that compiler's.

* It is keyed by its module's sources and the compiler's own build, so a different compiler never reads it.
  The interfaces of the modules it uses are checked when it loads rather than named by its key, so one artifact serves every edit of them that keeps their interfaces.
* So it has no format version and no migration, and its layout follows the checker's tables.
* A miss, a corrupt artifact and a truncated one all check the module again from source: deleting every artifact never changes a result.
* Only a module whose check reported nothing has one; a module that reports is checked from source by every compile, which reports it.
* The compiler writes and reads bytes, and where they are kept is its host's: slib's persistent cache, a table a frozen build embeds, or nowhere.
  sgl depends on clean-core alone, so the store is never sgl's.

**No process keeps an artifact for another by default; keeping them is a capability a host turns on.** *Decided with the maintainer on 2026-11-09.*
A compile reads nothing but its arguments, which is what makes its result cacheable, and it is synchronous.
So a compile never asks a store: a store behind it would be file I/O inside the compile, and a passive wait on it a workaround.
A host that keeps artifacts fetches them before the compile, asynchronously, and hands the loaded layers in; it saves what a compile made the same way after.
By default no host does: what a kept prelude bought was about 30 ms of a 120 ms `sgl describe`, in a process whose start dominates anyway.
Live editing keeps its layers in memory for the whole process, and a batch mode would pay the prelude once for many sources.

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
* A dependent whose own check read a body of an import, a generic it instantiated or a function it evaluated, records it.
  It moves only past an edit that left that import's sources alone; naming the body's own hash would be the finer grain.

**Options are a second layer.** *Decided with the maintainer on 2026-11-09.*

* An option never changes the names a module declares: nothing at file scope is conditional, `const if` being a statement (AST-157).
  What it changes is types, constants and bodies: an array's length, an image format, a workgroup size, a `const` computed from it, a branch on it.
* So a module's layer is two: the first holds its names and everything that reaches no option, checked once at the defaults.
  The second is everything that reaches one, a fixed point over the declarations whose types, constants or bodies name an option, through other modules' declarations too (CHK-355).
* A compile that sets options checks the second layers it reaches again, for its values and over the first layers; the first are never checked again.
  A module that declares no option, most of them, has an empty second layer.
* A rule that judges a constant judges the values the compile gives, as it does today (CHK-354): a module's error under one value is found by the compile that gives it.
* An artifact holds only the first layer, and the early cutoff keys on its interface; a second layer is a compile's own and never kept.
* Folding an option into a body only when it is inlined, as the target is (CHK-426), would keep such bodies out of the second layer.
  It is set aside: it covers no type, which a second layer must handle anyway, and few modules declare an option at all.

*A first form landed on 2026-11-09:* a compile that sets only its own program's options continues from its modules' layer (`check::layer_serves_options`).
No module sees its program, so those options change nothing the layer checked; one that sets a module's option still checks every module itself.
Every module in the tree declares no option today, so this first form is all a compile here needs.

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
   *Since 2026-11-09 the library is one layer, `check::check_library`, which every program reaching any of its modules continues from.*
   Ids stay positions: a module the program does not use is checked into the layer too, and nothing of the program sees it.
   A pass over every symbol, pipeline or require line asks `checked_module::is_reached_file` first, and so does a name an emitted name must avoid.
   `reached_library_files` stays what the program reaches, which is what an edit of a library file changes it for.
   shaped-viewer's 12 programs reach 10 distinct sets of its 10 modules: checked a set at a time 139 ms in a process, the library once 27 ms (relwithdebinfo).
   A test holds every one of those programs to checking as it does beside the modules it reaches alone, the emitted text of every target included.
   Where a module reports anything the library makes no layer, and the driver keeps one per set of modules as before.
   An edit of any module checks the library again, about 27 ms there; one layer per module, with relocation and early cutoff, is what would cut that.
   The language server continues from the same layers, `sgl::layer_of`, so a keystroke in a shaped-viewer program is a 3.6 ms analysis (release).
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

What is left, decided with the maintainer on 2026-11-09, in this order:

6. **A chain of module layers.** The library's modules in one order, dependencies first, each module a layer over the one before.
   Within one library that order is fixed, so ids stay positions and nothing is relocated.
   An edit of a module checks it and the modules after it again, and none before: the order puts a module nothing else uses last.
   *Landed on 2026-11-09:* `check::library_chain` orders them, `check::check_next_module` checks one over the layer before, and `check_library` is the chain whole.
   The driver keeps each link under a key of the modules up to it, so a library that changed a module finds the links before it.
   shaped-viewer's chain is 24.7 ms whole (release); an edit of its first module checks it all, of its last 1.4 ms, 13.8 ms on average.
   In the language server a keystroke in its last module is a 5.9 ms analysis, and in its first 31.7 ms.
   Every link holds a whole copy of the tables below it, which [perf-sgl-layer-freeze-copies](../../issues/perf-sgl-layer-freeze-copies.md) is.
   *Since 2026-11-09 a link's lists share what is below it*, as a few segments of geometric size; its maps and the checker's other state are still copied.
   shaped-viewer's 10 links load in 4.1 ms rather than 8.6, the 9 after `scene` move in 3.0 ms rather than 4.5, and the language server's benchmark peaks at 246 MiB rather than 369.
   *Since 2026-11-09 the order follows the edit:* a library made under the same names as the one before marks the files whose sources changed.
   Their modules go as late as the modules using them allow, so every keystroke after the first checks the module and the ones after it, those using it among them.
   shaped-viewer: an edit of a module so placed is 8.6 ms on average, against 13.6 ms in the order of the chain alone; of `scene`, 14.4 ms against 24.3 ms.
   What remains is checking the modules using it again when its interface did not change, which is early cutoff (step 7).
7. **Early cutoff, through relocation.** A layer splits into its interface and its bodies, and a dependent's layer names its imports' interfaces.
   An edit that leaves a module's interface alone relocates the layers after it onto the new one rather than checking them again.
   That is the import table and the relocation pass of [ids](#ids), and what a dependent's key is made of.
   *Landed on 2026-11-09, in the chain's terms:* `check::relocate_link` moves a link checked over the old links onto the new ones.
   A link's part of every list is contiguous, so an id below the first changed part stays, and one in a moved part moves with it.
   A link's interface is its part once every signature and default is checked, before any body: a watermark, hashed with no place in a file in it.
   An id into what a changed link no longer holds refuses, and so does a link that inlined or instantiated a changed link's function (`bodies_read`).
   So does an entry a fresh check would have found in the new link instead, an interned type the new link holds under the same spelling.
   A link checked again into what it was, positions aside, keeps every entry, so one refusal does not cascade.
   A test holds every relocated link to what a fresh check makes, field for field, over shaped-viewer's modules and a library made to refuse, past three kinds of edit.
   shaped-viewer: past an edit of `scene` that keeps its interface, the 9 links after it relocate in 4.5 ms where checking them took 23 ms; a keystroke there is a 9.6 ms analysis, against 27.8 ms.
   *Since step 9* a move writes the link's own part as an artifact's bytes and reads them back over the new links, so every move in memory is an artifact's load too.
   A compile that sets a module's option moves the links after that module from the defaults' chain where they may.
   An option's value is part of its module's interface, so a link naming that module is checked again.
   What it does not do yet: keep the part of a changed interface that did not change, or move a link without copying the tables below it.
8. **Options as a second layer** (see [artifacts](#artifacts)): a compile that sets a module's option checks only what reaches it.
   *A first form landed on 2026-11-09:* a compile that sets only its program's own options continues from the layers, which is every compile in the tree.
   *A second, on 2026-11-09, in the chain's terms:* a link is checked with the values a compile gives its own module's options, and they are part of its key.
   So a compile that sets one shares every link before that module's, checks it and the links after it with its values, and a permutation compiled again finds them.
   The chain puts a module with an option as late as the modules using it allow, so few links come after it.
   What is left is the declaration's grain, which needs no link after the module's to be checked again: that is relocation's (step 7).
9. **Per-module artifacts, kept where a host asks:** each module's first layer, keyed by its sources and its imports' interfaces, relocated on load.
   The host's seam is async, and a compile is handed loaded layers.
   *Landed on 2026-11-09, in the chain's terms:* `check::save_link_artifact` writes a link's own part and the parts of the chain it was checked over.
   `check::load_link_artifact` reads it over a chain as it stands now, moved past every link that changed since, by the rules of step 7; a link of other modules or other files is a miss.
   Each link keeps where the parts of its chain lie, their two digests and a hash of each module's sources, which is what a load matches the chain it lands on against.
   A string the part holds is a place in its own files or the prelude's, and in another module's file a place and its text, pointed back into that file where the text there is the same.
   The driver's seam is `sgl::module_artifact_keys`, `offer_module_artifacts` and `take_module_artifacts`, behind `keep_module_artifacts`, which is off.
   A host fetches the keys' bytes before a compile, asynchronously, and hands them in; a compile loads a link from them where it would check it, and reads nothing else.
   A key is the compiler's build, the module's name and its files' sources, so it holds nothing of the modules it uses.
   shaped-viewer's 10 links load in 8.6 ms from 1.4 MB, where checking them takes 27.9 ms (release), each load copying the tables below it as a check does.
   *slib is the first host, on 2026-11-09:* `sgl_compiler_options::keeps_module_artifacts`, off by default, keeps them in its persistent cache.
   It fetches them when a package or a module directory is added, never during a compile, and `shader_library::prefetch_modules` is that fetch to await.
10. **Parallel layers**, and bodies on demand for a reader that only checks, if a measurement asks for them.

### Open in this design

* Whether the per-stack relocation shows, which would bring back ids that name their layer.
* How a declaration records an option it reaches through another module's declaration, which the overlay needs across artifacts.

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
