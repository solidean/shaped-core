# SGL Compiler Architecture

For whoever works **on** the compiler.
Whoever works **with** the language wants the [spec](spec/_index.md), the [example](../../../../examples/graphics/sgl-cube/shaders/cube.sgl) and the `sgl` tool instead.

## Why a language of our own

The alternatives each fail on something this repo needs, which is what the next reader of this library asks first.

* **HLSL through Tint or naga** reaches WGSL and MSL, and neither translator supports ray tracing.
  Both are large dependencies besides, and live editing in the browser would need them in the page ([portable HLSL](../../shaped-shader-library/docs/portable-hlsl.md)).
* **Slang** reaches every target but supports no ray tracing on Metal.
* **WGSL as the source** does not cover the feature set, so building on it means extending it anyway, which is a language of our own with someone else's grammar.
* **The binding model is ours**: a group numbered by its place in an entry point's list, and host code generated from the one parser of the source, need a language that states both.
* **Polyfills**, such as ray tracing on WebGPU, need a compiler that owns what it writes.

## The pipeline

```
bytes → line tree → tokens → group tokens → form tree      sgl::parse          → sgl::parsed_file
      → declarations, statements, expressions              sgl::ast::build     → sgl::ast::file_ast
      → names, types, evaluation, inlining                 sgl::check::check   → sgl::check::checked_module
      → structured form → core form                        sgl::check::legalize
      → text for one entry point                           sgl::emit::emit
```

`sgl::compile_to_text` is the whole pipeline in one call, against the prelude the library carries, so compiling needs no file.
`sgl::describe` stops short of text: it runs the same front end and the emitter's own checks, and hands back what a host is generated from.
It is what slib's package generator reads, through `sgl describe`, and it describes exactly what the emitter would build.

**The library answers every question, and a tool translates.**
What a compiler, an editor or any other tool asks about a source — what a token is, what type a binding got, which tests passed where — is a function of this library.
`sgl lsp` and the other commands of the `sgl` tool are shallow shims over it, so a second front end never grows in a tool.
Everything here is UTF-8 and byte offsets; an encoding a protocol wants, such as LSP's UTF-16 columns, is that protocol layer's.

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

**Two highlighters copy the syntax, and a syntax change updates both.**
One is the VS Code grammar, [sgl.tmLanguage.json](../tools/vscode-extension/syntaxes/sgl.tmLanguage.json).
The other is the review tool's Pygments lexer, [sgl_lexer.py](../../../../tools/review/lib/render/sgl_lexer.py).
A new keyword, operator, literal form or line-tree rule is not done until both draw it.
Both colour as control keywords the builtins whose names alone constrain control flow: `ddx`, `ddy`, `fwidth`, the barriers, `workgroup_uniform_load`, and the subgroup and quad operations.
Those last two are listed by exact name, never by prefix, since `quad_count` is an ordinary name.
The review tool's self-test holds the lexer's keywords to the form parser's `sgl_keywords`, and both highlighters' subgroup and quad names to the prelude's.
Nothing else is checked yet ([chore-sgl-highlighter-agreement-test](issues/chore-sgl-highlighter-agreement-test.md)).

## The check pass

**Name resolution, type checking and evaluation are one demand-driven pass** per module.
A symbol is untouched, in compilation, checked or failed, and one that is reached while in compilation is a dependency cycle.
Overloads resolve by type, operators among them.

It yields side tables over the untouched AST, which is what an editor asks about, and one flat typed tree per entry point, which is all an emitter reads.

**A file is named by its position.**
`check` takes the prelude's files, then the library files of the modules the program reaches, then the program's, so the program is the LAST file and never "file 1".
A diagnostic, an origin and a side table all name a file that way, and the front end is what turns a position back into `builtins.sgl`, a library file's name or the source's own.

**Modules are planned before anything is declared** (`check/check_modules.cc`).
The library's files are grouped by their `module` line, the program's `use` lines are followed to the modules it reaches, and a loop among them is reported.
Each module then has a scope of its own over the prelude's, and `m.name` looks into it through the `use` that named `m`.
Checked beside the library, everything reached is checked in one pass with the program.
Behind the driver, the library is a chain of layers instead, one per module (`check::library_chain`, `check::check_next_module`), so each module is checked once per process.
A module the program does not use is in the chain too, so a pass over every symbol, pipeline or require line asks `checked_module::is_reached_file` before it counts one.
An edit of a module checks its layer again and moves the ones after it (`check/relocate.cc`); [the compilation model](spec/incubator/compilation-model.md) has the rules.
**A new list or a new raw position in a record of the check pass is one the relocation must know**: an id names a table by its type, and an integer `relocator` reads by hand.
The test "a link relocated past an edit of a module" holds every relocated link to a fresh check's bytes, and `check::layer_difference` names the field where one goes wrong.

**The library's prelude is checked once per process.**
`sgl::checked_prelude()` is `check::check_prelude` over `parsed_prelude()`: the checker's whole state after the prelude, behind an empty program file.
Every driver continues from a copy of it with `check::check(checked, program)`, which runs only what the program adds, so every id the program makes comes after the prelude's.
A prelude that reports anything is never kept, and a custom prelude, or a source that stands in for a prelude file, takes the whole pass.
It stays one pass because nothing the prelude checks depends on the program: a lookup from a prelude file, operators included, sees the prelude alone.
`tests/check/checked-prelude-test.cc` compares the two paths over the corpus, the samples and sg's shaders.

**A checked layer can be written as bytes and read back** (`check/artifact.hh`), the prelude's and each set of modules' alike.
No process keeps one for another: a compile reads nothing but its arguments, so it does no I/O and stays synchronous.
`save_artifact` writes the checker's state after the prelude as bytes, and `load_artifact` makes that state again, a copy rather than a check.
One walk does both, and it walks each struct by its fields (`check/impl/fields.hh`), so a field added to the checker's state is in every artifact with nothing listed.
Such a struct is an aggregate with no C array field, and one the walk cannot take fails to compile there.
The same test file runs every program behind the layer read back and requires the very checked module, ids included.

**A diagnostic judged of an inlined body belongs to the entry points it was judged of** (CHK-402).
The flattener and the uniformity pass judge each entry point's flat tree, so their diagnostics carry `located_diagnostic::entry_points`, and identical ones merge into one that names them all.
Their details name the stage rather than the entry point, which is what lets a helper two entry points share yield one.
An index into a binding array is judged last, by `judge_indices`, over every copy the entry points inlined of it (CHK-401).
The front end keeps those apart as `entry_diagnostics`, so `compile_to_text` reports the program's and its own entry point's, and `describe`, which serves them all, every one.
A file-level reader, `sgl test`, the corpus and the language server, reports them all.

**The check pass is a tracer.**
It carries what its samples need, and every other construct is the one diagnostic `unsupported-yet`, never a guess.
[semantics/checking.md](spec/semantics/checking.md) says what is carried.

**Every call resolves through one routine, and records what it chose.**
Free calls, dot calls, operators, constructors and literal conversions all go through `checker::resolve_overload`.
It collects candidates by name and by the first argument's type scope, binds the arguments, measures a conversion chain per argument, and ranks.
The winner is kept as a `call_record`: the arguments in the order written, and which one fills each parameter.
The flattener reads that record and never the argument list, so named arguments, defaults and receivers need no second path.

**A call is defined by substitution, and every call of the program is inlined.**
A block named after the callee stands where the call stood, its arguments bound at the top in the order written, then its defaults, each `return` a `leave`.
A builtin call whose parameters take the arguments in another order, or two of whose arguments have an effect, binds them to lets first.
Beyond that the flattener never hoists and never reorders, since evaluation order is the legalizer's job.
Each body is checked once on its own, and each default once where it is declared.

**A branch on a constant is flattened on the side it takes, and nothing else** (CHK-356).
The flattener folds an `if` whose condition is a constant, a literal argument included, so the other side never enters the tree.
The text, the footprint, the uniformity pass and the features an entry point needs all read that tree, so none of them sees it.

**An option is substituted where its `const` compiles**, so every reader of a const reads the compile's value unaware.
`checker::options` holds the values, and `option_uses` records where the program names an option.
An entry point's options are the uses inside its declaration, its callees', its bindings' and the structs any of those name, which no value changes.
A module's option is matched by its qualified name, `checked_module::qualified_name_of`, which `describe` reports too.

**The target and the device are inputs of a check, as the options are** (CHK-426).
`check::compile_target` is what `check` takes beside them, and the flattener substitutes it where an inlined body reads `target_api`, `target_language` or `has`: `flattener::compiled_for`.
So a source is checked once per target, and `compile_all_to_text` checks once for each target it writes.
A test is flattened for no target, and a const that reads one is refused, which is what lets a layer of modules serve every target.
`describe` checks for no target, so a `const if` on one reports its `else` branch's footprint.

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
A new expression kind that names a binding also teaches `check/footprint.cc` whether it reads or writes it.

**The footprint is read from the core form**, in `check/footprint.cc`: what an entry point does to each slot of its bindings.
It is one pass over the entry point's node arrays, not a walk: `legalize` returns them compacted, every node reachable from `body` and every expression with one parent, and the pass asserts that.
A binding member counts as a read unless its parent says otherwise — the place of a store, or a builtin's parameter.
That is what makes it cover exactly what the emitted text uses, with no nesting limit of its own, which sg relies on to skip an untouched slot's layout transition.
`compile_to_text` and `sgl describe` both report it, and an entry point's `@expect(footprint = "...")` pins it (`check/check_footprint.cc`).

## Tests

**A `test` is a root of the check pass, and it has a flat tree of its own** in `checked_module::test_units`, of no stage and without a parameter.
Its body is checked after every function body, since a test in a function body is only found while that body is checked.
A check of a test and an `assert` anywhere flatten to one `check` statement, whose body leaves every node of the condition in a `var` of its own.
The interpreter reads those `var`s when a check is false, and `sgl::test::run_tests` narrows them into a report.
`legalize` removes every `check` first, which is all it takes for no target to write one.

`sgl::test_source` is the driver of a whole file's tests, what `sgl test` and the corpus run; `text_request::run_tests` makes a failing test an error of `compile_to_text`.
`@expect` is judged in the front end, once every phase's diagnostics are in one list.
An entry point's `@expect(error = "kind")` takes every diagnostic judged of its inlined body, wherever it stands, and such an entry point is never built, nor named by a pipeline (CHK-403).

## Builtins and the prelude

**A builtin lives in one place, a record of the C++ builtin registry**: its signature as SGL source text, its evaluator, its spelling per target, a type's layout per target.
The check pass, the interpreter, the emitters and the layout check look the record up, so none of them holds a list of builtins.

**A `@builtin` declaration meets its record by key**: a `struct` by its name, a `fun` by its name together with its parameter types.
So every overload is a record of its own, and `symbol::intrinsic` / `symbol::intrinsic_type` are positions in the registry (`builtin_id`, `builtin_type_id`).
The registry has no signature language of its own: `registry::finalize` parses the generated text with the normal parser and reads each name and parameter type back from it.
`checked_module::builtins` points at the registry a module was checked against, and `builtin_type_of` / `builtin_function` are how every reader gets from an id to a record.

**Registration is explicit and ordered**: `register_builtins` calls one function per topic, so the generated text is deterministic and no linker can drop a translation unit.
A family of overloads is a C++ loop that formats one signature per type, unless SGL's generics can say it once, in `prelude/core.sgl`.
`builtins::default_registry()` is the one cached value in the library: it is immutable, and building it parses the whole prelude.

**The prelude is four files.**
`prelude/builtins.sgl` is generated from the registry and committed, and `prelude/core.sgl`, `prelude/raytracing.sgl` and `prelude/tessellation.sgl` are hand-written SGL.
`raytracing.sgl` holds the trace, its vocabulary, and the software traversal webgpu runs ([spec/raytracing.md](spec/raytracing.md)).
`tessellation.sgl` holds D3D's fixed-function tessellator, for a device without one (CHK-527); a test reaches its `@internal` functions by appending to the file itself.
The compiler never opens the committed file: it generates the same text in memory, and the hand-written three are embedded when CMake configures.
The two texts are byte-identical, which is why a diagnostic's line and column are right in the committed file, and why `*.sgl` is `eol=lf` in `.gitattributes`.
A library test compares them as well, so a plain test run catches drift without the tool.
The `sgl-prelude` step of `dev.py check` refuses a generated file that differs from the registry, and `--fix` regenerates it.
[adding-a-builtin.md](adding-a-builtin.md) is the walk-through.

## The emitters

`sgl::emit::emit` writes one entry point as readable text for `hlsl_dx12`, `hlsl_vulkan`, `wgsl` or `msl`, with exactly the types and the binding it needs.
One walker reads the flat tree, and a target is a small spelling layer over it.
How a builtin is written comes from its record: a call under a name per target, an infix or a prefix operator, or a writer of its own for the few that are neither.
The size and alignment the layout rules place a value by, and each target's own, are fields of the type's record (`emit/impl/layout.hh`, `emit/impl/memory_form.hh`).

* Every target carries its **final addresses**: member order is the location, and an `@inline binding` sits where sg expects inline constants.
  HLSL writes a group's resources at the register or `[[vk::binding]]` sg's backends give its slot, so no binding pass reads SGL's text.
* A name that is reserved in one target gets a trailing underscore there.
  Every name a builtin writes is reserved from its record, so a local named `lerp` is renamed in HLSL without an entry in any list.
  A custom writer's own names, such as HLSL's `mul`, stand in its spelling's `hlsl_names`, `wgsl_names` and `msl_names`.
  An entry point is renamed the same way, and `emitted_text::entry_point` is the name a caller compiles.
* **The `msl` text compiles**, through `shaped-shader-compiler-msl` and slib's metal edge.

[semantics/emitting.md](spec/semantics/emitting.md) has the rules.

## The native layer

`ir/` is one entry point below the flat tree, which a binary target writes out with no compiler behind it: `ir::lower` fills an `ir::program`, and `ir::construct_ssa` promotes its locals.
`ir::state_constant_tables` then makes a local SSA left in memory a `storage::table` where it is a constant written once: an initialized `Private` variable on SPIR-V, an internal constant on DXIL.
`ir/pipeline.hh` is each target's whole way, one plain function per target, `ir::compile_spirv` and `ir::compile_dxil`; a pass joins a route there.
A stage's edge is `program::stage_elements`, filled by `ir/lower_stage.cc` from the plan, which `op::stage_load` and `op::stage_store` reach; `ir/dxil/signature.hh` places it in DXIL's rows.
Every stage but the ray-tracing ones lowers: a vertex or pixel entry reads its edge ahead of its body, and each `return` stores its outputs.
A geometry or tessellation entry reads an array parameter where the body indexes it, and a control stage's body is its patch-constant function, beside a pass-through each writer generates (EMIT-240).
A mesh stage stores its elements at the index `vertex` and `primitive` name, and a task stage's payload is one global, which a mesh stage reads through rather than copies (EMIT-242).
A `discard` is an operation with an effect and no terminator, so a pass keeps it in its branch and the path goes on as a helper.
A binding array is one global, and an `access_chain` picks the element, so DXIL makes that element's handle where the chain stands.
`ir::write_spirv` ends the first, whose writer stands in `ir/spirv/`, and `ir::write_dxil` the second, whose backend stands in `ir/dxil/`.
The optimizing passes stand in `ir/`, on values as they are, so each keeps every operation's statement and guarantee: `ir/passes.hh`, with `ir::simplify` beside it.
`ir::simplify` and `ir::flatten_branches` each take a `control_flow`, and keep structured control flow under `structured`.
There a selection `simplify` decides branches to one side and leaves the other unreachable, as SPIR-V allows, and `flatten_branches` folds only a selection whose arms head nothing.
SPIR-V's route runs both so, and `ir::regroup_lanes`, which gathers the phis SSA made per lane back into one phi of the vector.
Below them the DXIL module sweeps only what exists once a value is its lanes: `share_common_values`, `merge_buffer_loads` and `remove_unused` of `llvm_passes.cc`.
[native-parity.md](native-parity.md) is how both routes measure against DXC's, and what is left between them.
It reads the same `plan` a text target does, so it decides no name and no address, and it hangs off `emit_entry_point` rather than being a target of its own.
A program is flat tables of small ids, an operation being 24 bytes, because this path exists to be fast.
`ir::verify` says what every program must hold, and a compile runs it after lowering and after each pass wherever assertions are on.
Nothing else stands between a pass's mistake and a driver, since no validator runs on a shipped compile.
A query is a value of `type_kind::ray_query` that no store or phi moves, and `ir/lower_query.cc` lowers every step of it; `dxil/impl/writer_query.cc` is DXIL's half.
A ray-tracing stage's edge is `ir/lower_ray.cc`, and `dxil/impl/writer_ray.cc` is what a DXIL library adds: its function's parameters, its roots and its operations.
What has no lowering yet is one named gap, never a guess, and [epic-sgl-native-ir-targets](issues/epic-sgl-native-ir-targets.md) has the design and the numbers.
A builtin reaches it by its record's name rather than by a spelling of its own: `lower_call.cc`'s table maps a name's stem to what it does, once per process.

## The tool

`sgl` is the toolchain's command line, a nexus binary under `tools/sgl/` whose jobs are `COMMAND`s.

```bash
uv run dev.py run sgl -- emit <file> --entry <name> --target <hlsl-dx12|hlsl-vulkan|wgsl|msl> [--run-tests]
uv run dev.py run sgl -- test <file>...              # the tests of each file, on the interpreter
uv run dev.py run sgl -- describe <file>             # what slib's generator reads: bindings, edge structs, entry points
uv run dev.py run sgl -- prelude --check <path>      # exit 2, and where the texts part, when the file differs
uv run dev.py run sgl -- prelude --write <path>      # what `uv run dev.py check sgl-prelude --fix` runs
uv run dev.py run sgl -- bench <list> [--rounds N]   # compile speed in-process; shader-rosetta's sgl-benchmark.py drives it
```

A further tool of the toolchain is one more file with one more `COMMAND`, and `sgl` alone lists what the binary holds.
It is built wherever `SC_BUILD_TOOLS` is on, and the `sgl-prelude` step skips with a line where it is not.

## The spec is tested

Every `sgl` fence in the spec is a test, so the spec and the parser cannot drift apart silently.
A fence that holds a `test` runs it as well.

**The corpus is how the language's semantics are tested**: one `.sgl` file per topic under `tests/corpus/`, found when the test binary runs, so a new file needs no C++ and no CMake.
A file passes when it checks with no diagnostic at all, every test in it passes, and every entry point it declares is written for every target that has what it requires (EMIT-109).
A rule is best stated as a test of a corpus file; a C++ `TEST` is for what SGL cannot say, which today is bindings, resources and the emitted text.
A C++ test that compares a whole emitted text or tree dump uses `sgl_test::check_golden(actual, expected, label)`, since no log shows a long one whole.
Where it differs, the failure names a file of the text as it is now and one of the C++ literals that replace `expected`.
A corpus file is written the way SGL is meant to be written, since it is also what a reader learns the language from.
An arrow body leaves its return type to inference, and a written `-> T` stays only where the written type is what a test is about.
Rules have stable ids and are never renumbered; a new rule is appended.
Every "why" is mirrored in a `why/` folder beside its rules, and ideas that are not spec yet live under `spec/incubator/`.

## What does not exist yet

Lambdas beyond an arrow body, and `mut self`.
GLSL.
Iterative walks: `interpret`'s `eval` and the legalizer's expression walks recurse, so the smallest stack a walk runs on bounds `k_max_depth`.
That makes a 40-term sum `nesting-too-deep` (CHK-268).
Over an explicit work stack, with a cycle caught by an on-path bit rather than by depth, the limit could be far higher.
Separate compilation, interfaces and the parallel driver of [the compilation model](spec/incubator/compilation-model.md).
