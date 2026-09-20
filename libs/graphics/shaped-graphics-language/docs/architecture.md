# SGL Compiler Architecture

For whoever works **on** the compiler.
Whoever works **with** the language wants the [spec](spec/_index.md), the [example](../../../../examples/graphics/sgl-cube/shaders/cube.sgl) and the `sgl` tool instead.

## The pipeline

```
bytes → line tree → tokens → group tokens → form tree      sgl::parse          → sgl::parsed_file
      → declarations, statements, expressions              sgl::ast::build     → sgl::ast::file_ast
      → names, types, evaluation, inlining                 sgl::check::check   → sgl::check::checked_module
      → structured form → core form                        sgl::check::legalize
      → text for one entry point                           sgl::emit::emit
```

`sgl::compile_to_text` is the whole pipeline in one call, against the prelude the library carries, so compiling needs no file.

**Total and local by construction.**
Any bytes parse to a tree plus diagnostics, and no syntax error escapes its indentation.
Every later phase is total as well: a malformed input is a diagnostic or an error result, never an assert.

**Dependencies.**
The library depends on clean-core alone, the emitters included.
The syntactic half must stay that way, since an editor links it to parse.
slib links sgl, never the reverse.

## The syntactic half

One flat lossless `sgl::parsed_file` per file: `print_source` gives the bytes back.
Every tree is a value with typed ids (`enum class … : i32 { none = -1 }`) and a text dump the tests compare against.
The AST pass is per file and name-free: a name is a span, and nothing is looked up.

## The check pass

**Name resolution, type checking and evaluation are one demand-driven pass** per module.
A symbol is untouched, in compilation, checked or failed, and one that is reached while in compilation is a dependency cycle.
Overloads resolve by type, operators among them.

It yields side tables over the untouched AST, which is what an editor asks about, and one flat typed tree per entry point, which is all an emitter reads.

**A file of a module is named by its position.**
`check` takes the prelude's files and then the program's, so the program is the LAST file and never "file 1": behind the library's prelude it is file 2.
A diagnostic, an origin and a side table all name a file that way, and `compile_to_text` is what turns a position back into `builtins.sgl`, `core.sgl` or the source's own name.

**The check pass is a tracer.**
It carries what its samples need, and every other construct is the one diagnostic `unsupported-yet`, never a guess.
[semantics/checking.md](spec/semantics/checking.md) says what is carried.

**A call is defined by substitution, and every call is inlined.**
A block named after the callee stands where the call stood, its arguments bound at the top, each `return` a `leave`.
The inliner never hoists and never reorders, since evaluation order is the legalizer's job alone.
Each body is checked once on its own.

**Compiling a function means its signature, with one exception.**
A body is checked after every signature is known, which is what lets a function call one declared below it.
An arrow body without `-> T` infers its result, so its body is checked as part of compiling it, while the symbol is still in compilation.
Needing such a function from inside its own body is therefore a `dependency-cycle`, and one written return type on the loop turns it back into a `recursive-call`.
A call does not demand an overload whose parameters cannot take it, since parameters are known before a result is.
That is `checker::is_out_of_the_running`, and without it an overload set could not be used from inside one of its own inferred members.

## The flat tree and its two forms

**The structured form** is what the language means and what the check pass writes: labeled blocks that may be expressions, and `leave` from any depth.
**The core form** is what every target prints one to one, and `sgl::check::legalize` takes the first to the second.
[semantics/evaluation.md](spec/semantics/evaluation.md) is the normative meaning of a flat tree.
[semantics/legalization.md](spec/semantics/legalization.md) is the informative appendix with the rules and the per-target table.

`sgl::check::interpret` runs both forms.
Differential tests hold the legalizer to "behaves the same": a randomized one over trees, and one over programs written in SGL.
`uv run dev.py test "sgl legalize" --thorough` runs the randomized one at full strength.
`sgl::check::flat_builder` writes a tree by hand, for a test that wants a shape the source does not give.

**A new statement or expression kind touches a fixed set of places**, and `flat_eval`, a call whose value is dropped, is the worked example.
They are the variant in `check/flat.hh` with its forward declaration, `flat_builder`, the flattener, `check/dump.cc`, and `for_each_expr_of` in `legalize/impl/walk.hh`.
Then come `find_core_violation`, both legalizer passes, the interpreter and the shared text writer, plus one `dialect` method where the targets differ.
The random generator of `tests/legalize/random-program.cc` has to produce it too, or the differential test never meets it.
The legalizer drops an `eval` whose value was a block once the block has moved in front, since what is left is a read of a local; a call stays, pure or not.

## Builtins and the prelude

**A builtin lives in one place, a record of the C++ builtin registry**: its signature as SGL source text, its evaluator, its spelling per target, a type's layout per target.
The check pass, the interpreter, the emitters and the layout check look the record up, so none of them holds a list of builtins.

**A `@builtin` declaration meets its record by key**: a `struct` by its name, a `fun` by its name together with its parameter types.
So every overload is a record of its own, and `symbol::intrinsic` / `symbol::intrinsic_type` are positions in the registry (`builtin_id`, `builtin_type_id`).
The registry has no signature language of its own: `registry::finalize` parses the generated text with the normal parser and reads each name and parameter type back from it.
`checked_module::builtins` points at the registry a module was checked against, and `builtin_type_of` / `builtin_function` are how every reader gets from an id to a record.

**Registration is explicit and ordered**: `register_builtins` calls one function per topic, so the generated text is deterministic and no linker can drop a translation unit.
A family of overloads is a C++ loop that formats one signature per type, until generics can write it once in SGL.
`builtins::default_registry()` is the one cached value in the library: it is immutable, and building it parses the whole prelude.

**The prelude is two files.**
`prelude/builtins.sgl` is generated from the registry and committed, and `prelude/core.sgl` is hand-written SGL.
The compiler never opens the committed file: it generates the same text in memory, and `core.sgl` is embedded when CMake configures.
The two texts are byte-identical, which is why a diagnostic's line and column are right in the committed file, and why `*.sgl` is `eol=lf` in `.gitattributes`.
A library test compares them as well, so a plain test run catches drift without the tool.
The `sgl-prelude` step of `dev.py check` refuses a generated file that differs from the registry, and `--fix` regenerates it.
[adding-a-builtin.md](adding-a-builtin.md) is the walk-through.

## The emitters

`sgl::emit::emit` writes one entry point as readable text for `hlsl_dx12`, `hlsl_vulkan`, `wgsl` or `msl`, with exactly the types and the binding it needs.
One walker reads the flat tree, and a target is a small spelling layer over it.
How a builtin is written comes from its record: a call under a name per target, an infix or a prefix operator, or a writer of its own for the few that are neither.
The size and alignment the `layout-mismatch` check places a member by are fields of the type's record.

* The text carries its **final addresses**: member order is the location, and an `@inline binding` sits where sg expects inline constants.
  slib's binding pass is therefore not needed behind it.
* A name that is reserved in one target gets a trailing underscore there.
  The function a builtin is called as is reserved from its record, so a local named `lerp` is renamed in HLSL without an entry in any list.
  The exception is a function only a custom writer calls, such as `mul`, which stands in `emit/reserved_words.cc`.
  An entry point never changes, so one that collides anywhere is an error everywhere.
* **The `msl` text has met no Metal compiler yet**, and nothing builds it.
  slib has no metallib compiler, and sg's metal backend binds no vertex buffers or inline constants.

[semantics/emitting.md](spec/semantics/emitting.md) has the rules.

## The tool

`sgl` is the toolchain's command line, a nexus binary under `tools/sgl/` whose jobs are `COMMAND`s.

```bash
uv run dev.py run sgl -- emit <file> --entry <name> --target <hlsl-dx12|hlsl-vulkan|wgsl|msl>
uv run dev.py run sgl -- prelude --check <path>      # exit 2, and where the texts part, when the file differs
uv run dev.py run sgl -- prelude --write <path>      # what `uv run dev.py check sgl-prelude --fix` runs
```

A further tool of the toolchain is one more file with one more `COMMAND`, and `sgl` alone lists what the binary holds.
It is built wherever `SC_BUILD_TOOLS` is on, and the `sgl-prelude` step skips with a line where it is not.

## The spec is tested

Every `sgl` fence in the spec is a test, so the spec and the parser cannot drift apart silently.
Rules have stable ids and are never renumbered; a new rule is appended.
Every "why" is mirrored in a `why/` folder beside its rules, and ideas that are not spec yet live under `spec/incubator/`.

## What does not exist yet

Generics, methods, lambdas, `case` and enums.
GLSL, a Metal toolchain, the generated host mirror, and every binding that is not `@inline`.
Modules, interfaces and the parallel driver of [the compilation model](spec/incubator/compilation-model.md).
