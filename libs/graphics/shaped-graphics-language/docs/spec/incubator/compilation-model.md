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

**The prelude is a file on disk**, written in SGL, that the compiler reads and places in front of the program.
That needs no multi-file compilation.

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
* Whether an interface is a file on disk, and whether it is SGL text.
* Whether on-demand compilation is recursive or asynchronous.
* How a dependency cycle is reported when its loop passes through a type expression.
* Whether a file without a `module` line is a module of its own.
* Which target decides a question the targets disagree on, such as matrix layout or the depth range.
