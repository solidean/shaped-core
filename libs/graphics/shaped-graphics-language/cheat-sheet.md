# shaped-graphics-language cheat sheet

SGL's toolchain as a library: the syntactic half, from bytes to the form tree, the AST pass on top of it, and a tracer of the check pass and the emitters.
One call runs all of it, from a source to the text a graphics API compiles.
Namespace `sgl`.
Depends on clean-core.

The language itself is specified in [docs/spec/](docs/spec/_index.md); this sheet is the C++ API.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `sgl`, for what the *library* has to say about itself.
What the *user's source* did wrong is `sgl::diagnostic`, which is data a caller receives and never a log line.

## Source in, text out

```cpp
#include <shaped-graphics-language/driver/compile_to_text.hh>
auto const r = sgl::compile_to_text({.source = text, .source_name = "cube.sgl", .entry_point = "main_ps",
                                     .stage = sgl::check::stage::pixel, .target = sgl::emit::target::wgsl});
                                           // -> cc::result<emitted_source, cc::string>: the text, or what a reader is told
                                           // parse + ast::build + check against the library's prelude + emit, in one call
r.value().text                             // the emitter's text, unchanged
r.value().entry_point                      // the name the text declares, which is the one to compile: `main` is `main_` in MSL
r.value().bound_names                      // { emitted, host } per resource: `work_values` is what the host binds as `work.values`
r.value().color_targets  .target_struct    // a pixel entry point's target count and `@pixel struct`; -1 and empty otherwise
r.error()                                  // one line per diagnostic: `cube.sgl:12:5: error: unknown-name: foo`
                                           // one inside the prelude names `builtins.sgl` or `core.sgl`
                                           // a missing entry point names the ones the source holds; a wrong stage says both
sgl::text_request                          // source, source_name ("<sgl>"), entry_point, stage (none = any), target
                                           // the entry point is found by NAME; source_name is never opened

#include <shaped-graphics-language/driver/describe.hh>
auto const d = sgl::describe({.source = text, .source_name = "cube.sgl"});
                                           // -> cc::result<module_description, cc::string>: what the host side is generated from
d.value().bindings                         // name, is_inline, members (constant: offset + size; buffer: slot + host_name `work.values`), block_size
                                           // texture / image / sampler members also carry the sg enum values of their binding:
                                           // texture_dimension, sample_type, image_format + access, sampler_type, static_sampler
d.value().structs                          // the @vertex / @pixel structs: name, edge, members with their location
d.value().entry_points                     // name, stage, workgroup, bindings (the list as written)
d.value().pipelines                        // name, stages, layout, vertex_input, target_set, targets, settings, open (the `.host` paths)
                                           // bindings and structs carry `shape`: check::structural_hash of their members,
                                           // 32 hex digits; the type's own name is not in it. What a hot reload compares.
                                           // types are SGL spellings (`float3`, `mat4`); mapping them to a host is the reader's job
                                           // only the file's own declarations, and only what the emitter would build

#include <shaped-graphics-language/driver/prelude.hh>
sgl::prelude_files()                       // -> cc::span<prelude_file const> { name, source }, in module order:
                                           // "builtins.sgl": GENERATED in memory from the builtin registry, never read from disk
                                           // "core.sgl": the hand-written prelude/core.sgl as it was when the library was built

#include <shaped-graphics-language/source/format_diagnostic.hh>
sgl::line_column_of(source, offset)        // -> sgl::line_column { line, column }, both 1-based, columns in bytes
sgl::format_diagnostic(name, source, d, detail = {})   // `a.sgl:2:2: error: unknown-name: foo`; warning / error from d.level
```

## Parsing a file

```cpp
#include <shaped-graphics-language/syntax/parsed_file.hh>
auto const file = sgl::parse(source);      // -> sgl::parsed_file; TOTAL: any bytes in, a tree and diagnostics out
file.source                                // cc::string, the bytes every span points into
file.lines  file.tokens  file.groups  file.forms  file.diagnostics   // flat arrays; a node is named by a typed id
file.first_line  file.root_block  file.root_form                      // line_id / group_id / form_id: where each tree starts
file.at(id)                                // -> line / token / group / form (const&, or & on a mutable file); the id's TYPE picks the array
file.text_of(span)                         // -> cc::string_view into source
file.tokens_of(line)                       // -> cc::span<token const>
file.is_fused_left(token_id)               // -> bool: touches the token before it
file.form_attributes                       // cc::vector<group_id> of attribute groups; each form owns a contiguous range
file.form_attribute_arguments              // cc::vector<form_id>, PARALLEL to it: the round list of `@name(…)`, none for a bare `@name`
```

## The phases, one at a time

```cpp
#include <shaped-graphics-language/lines/line_tree.hh>
auto file = sgl::build_line_tree(source);  // lines nested by indentation, nothing else; kinds are blank / code
#include <shaped-graphics-language/tokens/tokenizer.hh>
sgl::tokenize(file);                       // settles line kinds (comment, string_content) and fills tokens
#include <shaped-graphics-language/groups/grouper.hh>
sgl::group_tokens(file);                   // parens matched, lines folded, attributes attached -> file.root_block
#include <shaped-graphics-language/forms/form_parser.hh>
sgl::parse_forms(file, sgl::default_keywords());   // -> file.root_form; the keyword table is the only language knowledge
// Each phase is called ONCE, in this order, and only reads the arrays of the phases before it.
```

## The trees

```cpp
#include <shaped-graphics-language/syntax/ids.hh>
sgl::line_id  sgl::token_id  sgl::group_id  sgl::form_id   // enum class : i32, one per array; `none` (-1) is the absent link
sgl::is_valid(id)                          // -> bool: not `none`
sgl::index_of(id)                          // -> isize, for a side array parallel to the id's own; explicit casts stay rare
sgl::next(token_id)  sgl::previous(token_id)   // tokens are consecutive within a line; NO other id has arithmetic
for (auto c = file.at(id).first_child; sgl::is_valid(c); c = file.at(c).next_sibling)   // how every tree is walked

sgl::line    // text, terminator_length, kind, indent_bytes/indent_columns, parent/first_child/next_sibling (line_id),
             // first_token/token_count (u32 range into file.tokens), tokens_begin()/tokens_end() -> token_id
sgl::token   // kind + where; NO keyword/number/identifier kinds: `10a7`, `@range`, `let` are all token_kind::symbol
sgl::group   // kind (token, round, square, curly, quoted, block, statement, attribute), is_fused_left, starts_line,
             // token/close_token (token_id), first_child/next_sibling/first_attribute (group_id)
sgl::form    // kind, where, token (token_id), first_child/next_sibling (form_id),
             // first_attribute/attribute_count (u32 range into file.form_attributes)
sgl::source_span   // { u32 offset, length }; end(), empty()
```

`form_kind`: `missing` `error` `number` `quoted` `hash_literal` `identifier` `wildcard` `keyword` `op`
`round_list` `square_list` `curly_list` `leading_dot` `member` `call` `application`
`prefix_operator` `postfix_operator` `operator_run` `keyword_form` `block` `sequence`.

## The AST

```cpp
#include <shaped-graphics-language/ast/build.hh>
auto const ast = sgl::ast::build(file);    // -> sgl::ast::file_ast; TOTAL, and `file` is not modified
ast.exprs  ast.stmts  ast.decls            // the three families; a node is { form, attributes, node (cc::variant) }
ast.fields  ast.arguments  ast.attributes  ast.case_arms  ast.if_branches     // list elements and parts
ast.expr_lists  ast.stmt_lists  ast.decl_lists  ast.token_lists               // child lists of ids
ast.declarations                           // range_of<decl_id>: the file's declarations, in source order
ast.diagnostics                            // the AST pass's own; the syntactic ones stay in file.diagnostics
ast.at(id)                                 // -> expr / stmt / decl / field const&; the id's TYPE picks the array
ast.at(range)                              // -> cc::span<T const>; range_of<T>'s T picks the side array

#include <shaped-graphics-language/ast/ids.hh>
sgl::ast::expr_id  stmt_id  decl_id  field_id   // enum class : i32, `none` (-1) is the absent link
sgl::ast::is_valid(id)  sgl::ast::index_of(id)
sgl::ast::range_of<T>                      // { u32 first, count }; empty()

ast.at(id).node.is<sgl::ast::call>()       // which kind; try_as<T>() -> T const*, visit(…) for all of them
sgl::ast::argument   // form, attributes, name (empty = positional), value, is_splat, is_shorthand
sgl::ast::field      // form, name, is_mut, type, default_value, attributes — fields, members, EVERY parameter
sgl::ast::body       // kind (none / block / arrow), form, statements, value
sgl::ast::attribute  // group, name (without `@`), list (the round list form, or none), arguments (range_of<argument>)

#include <shaped-graphics-language/ast/dump.hh>
sgl::ast::dump(file, ast)                  // (fun f (params (field x : int)) -> int => (call:infix + x num:1))
sgl::ast::dump_diagnostics(ast)            // `stray-else @6+4`, one per line
```

Expressions: `invalid_expr` `literal` `name` `self_ref` `wildcard` `leading_dot` `member` `index` `call` `tuple` `array` `object`
`comparison_chain` `cast` `membership` `ascription` `range` `lambda` `case_expr` `loop_expr` `return_expr` `yield_expr`
`break_expr` `continue_expr` `struct_type` `function_type` `with_bindings`.

- `call` is every application: `spelling` is `paren` / `juxtaposition` / `infix` / `prefix`, and an operator call has `op` instead of `callee`.
- `index` is a fused `a[…]`, subscript or type application alike.
- `comparison_chain` is two or more comparisons; one comparison is an infix `call`.
- `range` is `a ..< b` / `a ..= b`, a node of its own.
- `struct_type` is a curly list of `name: type` elements only; any other curly list is an `object`.
- An `object` element is `name`, `name = value` or `..splat`; anything else is kept positional and reports `expected-object-element`.
- `lambda` has two spellings, recorded in `spelling`: `arrow` for `x => …`, and `fun` for the anonymous `fun (x) => …`.
  Only the `fun` spelling has `type_parameters`, `bindings` and a `return_type`, and only it can be left with `return`.
- `function_type` is every `a -> b` that is not the return arrow of a signature; its form is a two-operand run.
- `with_bindings` is `f(x){…}`, reserved and always reported.

Statements: `invalid_stmt` `let_stmt` `assign_stmt` `if_stmt` (the whole chain, as `if_branch`es) `for_stmt` `while_stmt`
`assert_stmt` `print_stmt` `decl_stmt` `expr_stmt`.

Declarations: `invalid_decl` `module_decl` `use_decl` `fun_decl` `struct_decl` `enum_decl` `type_decl` `const_decl`
`binding_decl` `sampler_decl` `notation_decl`, and the member lines `field_decl` `property_decl` `enum_case_decl`.

## The builtin registry (the ONE place a builtin lives; [docs/adding-a-builtin.md](docs/adding-a-builtin.md))

```cpp
#include <shaped-graphics-language/builtins/registry.hh>
auto const& r = sgl::builtins::default_registry();   // make_registry(), built once per process and immutable: the one cached value
sgl::builtins::make_registry()             // -> registry: register_builtins(r) + r.finalize(); a VALUE, nothing registers itself
r.types  r.functions                       // type_record / function_record, named by builtin_type_id / builtin_id (enum class : i32)
r.prelude_text()                           // -> cc::string: prelude/builtins.sgl byte for byte, in registration order
r.find_type("float3")                      // -> builtin_type_id, none for an unknown name
r.find_function("dot", {vec3, vec3})       // -> builtin_id: ONE OVERLOAD is one record; the key is name + parameter types
r.at(id)  r.is_known(id)  sgl::is_valid(id)  sgl::index_of(id)

sgl::builtins::function_record             // signature (SGL SOURCE TEXT, without @builtin), doc (`///` lines), evaluate, write;
                                           // name / parameters / result are READ BACK from the parsed signature by finalize()
sgl::builtins::spelling                    // kind: call (text or .hlsl/.wgsl/.msl rename it; empty = the SGL name), infix, prefix, custom
sgl::builtins::infix("+")                  // an operator at the level it has in every target
f.called_in(language::hlsl)                // "lerp" for mix
sgl::builtins::evaluator                   // void(span<scalar const> in, vector<scalar>& out): every argument's scalars, back to back;
                                           // the interpreter checks counts and kinds, so ONE evaluator serves a whole family
sgl::builtins::custom_writer               // written(call_context const&): arguments already written, the target language, the registry
sgl::builtins::type_record                 // declaration (SGL text), doc, hlsl / wgsl / msl names, *_layout { size, alignment } per target
                                           // (size 0 = no place in a block), leaf_kind + leaf_count, crosses_edges
sgl::builtins::written { text, binds }  wrapped(w, needed)  write_infix(op, level, lhs, rhs)   // text plus its precedence

#include <shaped-graphics-language/builtins/register.hh>
sgl::builtins::register_builtins(r)        // register_types, _scalar_math, _vector_math, _transforms, in that FIXED order
impl::add_infix(r, "+", "add_vec3", "vec3", "vec3", "vec3", eval)   impl::add_negate(…)
impl::add_function(r, "mix", {"a", t, "b", t, "t", "float"}, t, eval, {.hlsl = "lerp"}, "/// doc")
                                           // a FAMILY is a C++ loop over type names that formats one signature each
```

## The check pass (a tracer: it carries `tests/samples/*.sgl` but basic-raster, everything else is `unsupported-yet`)

```cpp
#include <shaped-graphics-language/check/check.hh>
auto const m = sgl::check::check(prelude_files, {.file = user, .ast = user_ast});   // + a registry; default_registry() without
                                           // -> sgl::check::checked_module; TOTAL; a module_file is two REFERENCES
                                           // prelude_files: cc::span<module_file const>, parsed from sgl::prelude_files() or a test's own
                                           // file i is prelude file i, and the program is the LAST file; never concatenated
                                           // carried: let / let mut, assignment and `op=`, if chains, while, for over `a ..< b`, loop with
                                           // break / break value / continue, and / or / not, comparison chains, int literals, print,
                                           // and calls of the program's own functions, overloads included, which are INLINED
m.symbols                                  // every top-level fun / struct / binding: file, declaration, kind, state, name,
                                           // intrinsic (builtin_id) / intrinsic_type (builtin_type_id), operator_spelling, type, info
m.builtins                                 // the registry those ids are positions in; m.builtin_type_of(type_id) / m.builtin_function(id)
                                           // -> the record, or null
m.types  m.members                         // canonical types; types[0] is the error type, types[1] (nothing_type) what a fun without
                                           // `-> T` returns; fields and binding members
m.functions  m.parameters  m.binding_lists // signatures; symbol::info is the position in functions / bindings
m.bindings                                 // binding_info { symbol, is_inline, members }
m.samplers                                 // sampler_state per `sampler name:` block of a binding; member_info::static_sampler indexes it
                                           // resource types (texture / image / sampler) are interned like buffers; name_of spells them
                                           // `out image_2d[.rgba8_unorm]`; check/resources.hh holds the shapes and the image formats
m.files[f].type_at(expr_id)                // side table: type_id, none for what nothing checked
m.files[f].target_at(expr_id)              // side table: { kind, symbol, index } — local / parameter / symbol / overload /
                                           // constructor / field / binding_member
m.entry_points                             // flat_entry_point per SOUND entry point, in the STRUCTURED form; what an emitter reads
                                           // sound means: its body and the body of every function it reaches reported no error
m.diagnostics                              // located_diagnostic { what, file, detail }, in the order they were found
m.at(symbol_id)  m.at(type_id)  m.at(range)  m.name_of(type_id)   // name_of gives "<error>" for the error type

#include <shaped-graphics-language/check/flat.hh>
e.entry_stage  e.name  e.input  e.result  e.bindings   // stage, the name as written, edge structs, the LISTED bindings
e.locals  e.exprs  e.stmts  e.body         // locals[0] is the parameter; at(id) / at(range) like the AST
sgl::check::flat_expr                      // { type, from (origin), inlined_through, node }: flat_literal (float), flat_int_literal,
                                           // flat_bool_literal, flat_enum_value, flat_local_ref, flat_binding_member, flat_member,
                                           // flat_buffer_element (`values[i]`, a place when stored to), flat_construct,
                                           // flat_call { callee, intrinsic, is_pure, arguments }, flat_not, flat_and, flat_or,
                                           // flat_block (a block EXPRESSION: structured form only)
                                           // a call of the program's function is a flat_block named after the callee: arguments bound by
                                           // `let`s at its top (a literal or an immutable local stands for its parameter), return = leave
x.from  x.inlined_through                  // the AST node it came from, and e.at(range) -> the call sites it came through, outermost first
sgl::check::flat_stmt                      // flat_let, flat_var, flat_assign, flat_print, flat_eval (evaluate and drop), flat_if, flat_loop,
                                           // flat_while, flat_for,
                                           // flat_continue, flat_return; structured only: flat_block, flat_leave, flat_case;
                                           // core only: flat_once, flat_break, flat_switch
e.labels  e.root                           // flat_label { name } per block / loop; label_id is a typed id like every other
                                           // root is what `leave` names to return; none on a tree the check pass wrote
e.names.mint("n")                          // -> "n", then "n_1", …; EVERY name an emitter writes comes from here
e.names.reserve("main_ps")                 // -> false when taken; for a name that must not change

#include <shaped-graphics-language/check/dump.hh>
sgl::check::dump(m)                        // symbols, then entry points: (let n : vec3 = (call normalize … : vec3))
sgl::check::dump_entry_points(m)
sgl::check::dump_entry_point(m, e)         // one tree, either form: nested statements indented, labels as `$name`
sgl::check::dump_diagnostics(m)            // `unknown-name @1:120+4 foo`: kind, file, span, detail
```

## The two forms of a flat tree

```cpp
// STRUCTURED: what the language means and what the check pass writes. `block $b` is a statement or an EXPRESSION,
//   `leave $b [value]` exits it from any depth, `continue $l` any enclosing loop; and / or short-circuit.
// CORE: what a target prints 1:1. No block, no leave: `once`, `break` (innermost once or loop), `continue`
//   (innermost loop, NO once in between), `return` at any depth, `&&` / `||` only over a right side without effect.
// docs/spec/semantics/evaluation.md is the meaning (normative), legalization.md the rules and the per-target table.

#include <shaped-graphics-language/check/flat_builder.hh>
auto b = sgl::check::flat_builder::create(m, {.name = "main", .input = frag, .result = float_type});
                                           // locals[0] is the parameter, e.root a label, every module name taken in the mint
b.type_named("int")  b.type_named("frag")  // -> type_id of a struct, builtin or not; none when the module has none
b.literal(0.5)  b.int_literal(3)  b.bool_literal(true)  b.local(id)  b.member(object, "x")  b.construct(type, {…})
b.call("add", {x, y})                      // by NAME, an operator function by its own; among overloads the argument types choose
                                           // result type and purity from the prelude's declaration
b.call_with_effect("saturate", {x})        // the same call as one that has an effect; the prelude declares none yet
b.not_(x)  b.and_(x, y)  b.or_(x, y)  b.block_expr(label, type, {stmts…})
auto const x = b.var("x", type, value);    // -> { local, stmt }; without a value it holds nothing. b.let("x", value) alike
b.assign(place, value)  b.print(v)  b.eval(v)  b.if_(c, {then…}, {else…})  b.block(label, {…})  b.leave(label, value)
b.loop(label, {…})  b.while_(label, c, {…})  b.for_(label, index_local, first, end, {…})  b.continue_(label)
b.once({…})  b.break_()  b.return_(v)      // the core-only statements
b.set_body({…});  b.e                      // a statement joins no list until a body names it; NOTHING is type checked
                                           // a span given to a method must not alias the tree (it grows while read)

#include <shaped-graphics-language/legalize/core.hh>
sgl::check::is_core(e)                     // the definition of the core form
sgl::check::find_core_violation(e)         // -> cc::optional<core_violation { reason, stmt, expr }>: the FIRST offending node
sgl::check::has_effect(e, expr_id)         // a call that is not pure, or a block

#include <shaped-graphics-language/legalize/legalize.hh>
auto const core = sgl::check::legalize(m, e);   // structured -> core, same behaviour; a core tree comes back UNCHANGED
                                           // E1-E4 (block expressions, pins, and/or, loop conditions), X1-X6 (exits)
                                           // X6: a block that ends in a loop is left by leaving the loop, so `break value` is a `break`
                                           // every name it adds is minted: pick_result, x_before, search_left, rows_continued
sgl::check::legalize_options               // { skip_pinning, skip_flag_tests }: break a rule on purpose, for the tests' teeth

#include <shaped-graphics-language/interpret/interpret.hh>
auto const o = sgl::check::interpret(m, e, {.parameter = value, .bindings = {…}}, {.fuel = 1'000'000});
                                           // runs BOTH forms; left to right, each operand once; f32 and wrapping i32
o.status                                   // ok, out_of_fuel, fell_off_the_end, type_error, uninitialized_read: never asserts
o.result  o.trace  o.detail                // value { type, leaves }; trace = every print, and every call with an effect
o == other                                 // status, result and trace; NOT the detail
sgl::check::zero_value(m, type)  sgl::check::leaf_count_of(m, type)   // a value is its scalars in field order; mat4 is 16
sgl::check::scalar::of(0.5f)  .as_float()  .as_int()  .as_bool()      // equality is on the BITS
sgl::check::dump(o)                        // `ok 1.5 | print 1 | print true`
```

## The `sgl` tool (`tools/sgl/`, a nexus binary of COMMANDs; built under `SC_BUILD_TOOLS`)

```bash
uv run dev.py run sgl -- emit shader.sgl --entry main_ps --target wgsl   # the text, or the diagnostics and exit 2
uv run dev.py run sgl -- prelude [--check <path> | --write <path>]       # the generated builtins.sgl; --check exits 2 on a difference
uv run dev.py run sgl -- describe shader.sgl                             # sgl::describe as JSON: what slib's generator reads
uv run dev.py check sgl-prelude [--fix]                                  # the gate over prelude/builtins.sgl
```

## Emitting

```cpp
#include <shaped-graphics-language/emit/emit.hh>
sgl::emit::target                          // hlsl_dx12, hlsl_vulkan, wgsl, msl: a text format PLUS a backend's addressing rules
                                           // msl is written and pinned, and has met NO Metal compiler yet
sgl::emit::all_targets()                   // -> cc::span<target const>
auto const r = sgl::emit::emit(m, 0, sgl::emit::target::wgsl);   // -> emitted_text; the isize is a position in m.entry_points
                                           // LEGALIZES that entry point first, since the check pass writes the structured form
                                           // ONE entry point per call: it, and exactly the structs and the binding it needs
                                           // TOTAL and deterministic; every target carries FINAL addresses, HLSL's registers too
sgl::emit::emit_entry_point(m, e, t)       // the same for a tree that is no entry point of m: a legalized one, a hand-built one
                                           // e must be CORE, or the result is the error `not-core` with the first violation
r.has_text()  r.text                       // text is empty when there are errors
r.errors                                   // emit::error { kind, symbol, detail }; the SAME for every target
sgl::emit::to_string(target)  sgl::emit::to_string(error_kind)   // "hlsl-vulkan", "not-core"
sgl::emit::dump_errors(r)                  // `unsupported a print, which no target writes yet`, one per line

#include <shaped-graphics-language/emit/reserved_words.hh>
sgl::emit::reserved_words(t)               // -> cc::span<cc::string_view const>: keywords, predeclared types, the functions the text calls
sgl::emit::is_reserved(t, "target")        // true for wgsl only; msl also reserves its whole standard library, and `main`
```

## Diagnostics

```cpp
#include <shaped-graphics-language/source/diagnostic.hh>
sgl::diagnostic            // { kind, level, where }
sgl::to_string(kind)       // -> the stable kebab-case name: "undelimited-string"
sgl::default_severity_of(kind)   // normal_error / fatal_error / warning
```

## Reading a tree by eye, and in tests

```cpp
#include <shaped-graphics-language/debug/dump.hh>
sgl::dump_lines(file)        // the line tree, indented by TREE depth, not by the source's own indentation
sgl::dump_tokens(file)       // `code: symbol(print) quote_open(")`; `~` joins a fused token
sgl::dump_groups(file)       // one line per statement; `|` starts an element line, `{@…}` follows a group with attributes
sgl::dump_forms(file)        // s-expressions: (run (kw kw:let id:x) op:= num:10)
sgl::dump_diagnostics(file)  // `undelimited-string @6+1`, one per line
sgl::print_source(file)      // == file.source for EVERY input: the lossless invariant
// Dump formats are for tests and eyes; they are not stable and must not be parsed.
```

## Gotchas

- **`parse` never fails.** Check `file.diagnostics`, and expect `form_kind::missing` and `form_kind::error` inside a tree.
- **Whitespace is not stored.** It is the gap between two token spans; comments are tokens and are gone from groups onwards.
- **A string is not a token either.** It is `quote_open`, then `string_body` pieces and interpolations, then `quote_close`.
  An interpolation is a `dollar` token followed by symbol and dot tokens, or by the tokens of its parentheses; a `quoted` group holds them all as children.
- **`line::opens_string`** says a line ends in an opening quote, so its children are string content and its next sibling owes the closer.
- **A number is not a token.** `1.5e-3` is five tokens that the form parser assembles; a sign directly on it is part of the literal.
- **Operator spacing is syntax.** Spaced on both sides is infix, fused on the right only is prefix, fused on both sides is an error — except ranges.
- **`operator_run` is flat** for one precedence level (operands and `op` leaves alternate); assignment, `=>` and `->` hold two operands and nest right.
- **`->` is a level of its own**, tighter than `: as in` and looser than ranges.
  So `x : (int) -> int` is `x : ((int) -> int)` in the form tree already, and `a -> b -> c` is `a -> (b -> c)`.
  The ladder, loosest first: `;`, assignment, `=>`, keyword form, `and or not`, comparisons, `: as in`, `->`, ranges, bit-like, add-like, mul-like,
  application, prefix and postfix, fused lists and members.
- **A form's attributes are not on its groups.** Read them through `first_attribute` / `attribute_count` into `file.form_attributes`.
- **A range start is a position, not an id.** `line::first_token` and `form::first_attribute` are `u32`, since an empty range starts at nothing.
- **The AST is name-free.** `build` never looks a name up, so `vec3` is a `name` and `texture_2d[rgba8]` an `index` wherever they stand.
- **`build` is total.** What has no reading is an `invalid_*` node that keeps its form, and `ast.diagnostics` says what was expected.
  A `missing`, `error` or postfix form becomes `invalid` WITHOUT a second diagnostic.
- **An AST name is a `source_span`.** Nothing is interned; read it with `file.text_of(span)`.
- **A type is an expression in a type position.** The members called `type`, `function_type::result` and `fun_decl::return_type` are the positions.
  Only there may an expression carry attributes.
- **`self` is a reserved name, not a keyword.** The form tree holds an identifier and the AST a `self_ref`.
- **`return`, `break` and `yield` with a keyword value are one form.** `return case x:` is a keyword form with TWO keywords, and the AST reads the rest as the jump's value.
  A lambda as the value arrives the other way round, `(yield x) => body`, since `=>` is looser than a keyword form; the AST puts it back together.
- **A block never yields by ending in a value.** `yield value` hands it on from the nearest value block: the body of a `case` arm, an arrow lambda or a property.
  The blocks of `if`, `for` and `while` in between are looked through.
  A `loop:` is not: `yield-in-loop`, since a `loop` is left with `break value`.
  `yield` is experimental in the spec, so expect its rules to move.
- **A one-line `=> value` body takes no jump keyword.** `_ => yield 1` is the warning `redundant-yield`, and `x => return x` is the error `redundant-return`.
  Both keep their `yield_expr` / `return_expr` node, so the dump shows what was written; a later phase reads the value through it.
- **A jump in the wrong body names the right keyword, a jump with no target names nothing.**
  `yield-in-function` is a `yield` that finds a `fun` body first, and `return-in-lambda` is a `return` in an arrow lambda's block.
  `jump-without-target` is a `yield` or a `return` with no `fun` around it at all, and a `break` or a `continue` with no loop inside its function.
  A lambda is a function of its own there: a `break` in a lambda block does not reach the `for` around the lambda.
- **The variable of a `for` may carry a type.** `for i : int in r:` fills `for_stmt::type`, and a pattern on the left is still `for-takes-name-in-range`.
  A `return` looks through `case` arms and properties, so `_ => return false` leaves the function around the `case`.
- **An attribute's arguments are list elements like any other.** `@slider(0, max = 1)` holds a positional and a named `argument`; `@name()` has a `list` and no arguments.
- **`no-effect` is a warning, and no statement is exempt.** A paren or juxtaposition call, a jump, a `case`, a `loop` and `invalid` have an effect; nothing else does.
- **An anonymous `fun` is a lambda only in expression position.** As a statement it is a function that lost its name and reports `expected-name`.
- **`type name = …` is a type position**, like the right sides of `:`, `->` and `as`; the AST dump writes it `(type name : …)`.
- **`true` and `false` are ordinary names** to every phase here.
- **The check pass is one demand-driven pass.** A symbol is untouched, in compilation, checked or failed, and reaching one in compilation is `dependency-cycle`.
  Compiling a function means its signature; bodies are checked after every signature is known.
- **Every body is checked ONCE, on its own**, so a broken function nobody calls still reports, and one called three times reports once.
  Inlining happens afterwards, from the side tables, and only for an entry point whose every reachable body is sound.
- **Recursion is `recursive-call`**, once per loop of calls, at the call that closes it: `a -> b -> a`.
- **Bindings are an effect.** A call needs the callee's `{…}` list inside the caller's, or it is `binding-not-listed` at the call; so an entry point lists what its shader reads.
- **Every path of a function that returns a value ends in a `return`**, or it is `missing-return`.
  A `loop:` without a `break` never ends; a `while` always may, whatever its condition.
  What follows a jump in its list is the WARNING `unreachable-code`.
- **A block is a scope.** A local ends with its block, and two blocks beside each other may reuse a name.
- **A later local shadows an earlier one**, in the same block or an enclosing one, parameters included, as in Rust (CHK-53).
  Its value still sees the one it hides; each is a local of its own, minted `x`, `x_1`, … in the text.
- **A local or a parameter may shadow a module-level or prelude name** (CHK-54), and behind it the name is the local everywhere.
  So `let length = length v` is fine, and `length w` after it is a call of a local; a type position holding it is `wrong-kind-of-name`.
- **The program's file may shadow a prelude name** (CHK-188): its `struct vec3` is no duplicate, and its `fun dot` joins the prelude's overloads.
  Where both have a function a call matches, the program's wins (CHK-192), so a prelude release adding its signature breaks nothing.
  Only two non-functions of one name in one file are `duplicate-declaration`.
- **`and`, `or` and `not` are no functions**, and a comparison chain evaluates each inner operand once: it is bound where it first stands.
- **Still `unsupported-yet`:** generics, `self` and methods, `mut` parameters, lambdas and function values, nested functions, `const`, `use`,
  a `for` over anything but `a ..< b`, a `let` without a value, an expression statement that is no call, `assert`.
- **An arrow body without `-> T` infers its result**, and a BLOCK body without one still returns nothing.
  Its body is checked as part of compiling it, so two such functions that need each other are `dependency-cycle`, not `recursive-call`.
  An overload whose parameters cannot take a call is not demanded by it, so an overload set works from inside one of its inferred members.
- **A call as a statement is `flat_eval`**: evaluated, its value dropped.
  `value;` in HLSL, `_ = value;` in WGSL, `(void)(value);` in MSL; the legalizer drops one whose value was a block, once the block has moved.
- **The error type is silent.** What did not check has `checked_module::error_type`, and nothing that meets it reports again.
- **`unsupported-yet` is never a guess.** Its detail names the construct, and the construct's type is the error type.
- **A `@builtin struct` is keyed by its name, a `@builtin fun` by its name AND its parameter types**, against the registry.
  An `@operator` function is found through its operator alone: no lookup sees its name, which is documentation and what a dump shows.
- **Adding a builtin touches ONE record**, then `uv run dev.py check sgl-prelude --fix` regenerates `prelude/builtins.sgl`.
  No emitter, interpreter or layout switch exists to extend, and a test adds `fract` to a registry of its own to keep that true.
- **The user file is the LAST file, not file 1.** Behind the library's prelude it is file 2; a test with one prelude file of its own still has it at 1.
- **An entry point with any error has no flat tree.** `m.entry_points` holds only what an emitter may read.
- **The check pass writes the structured form**, and only `once` and a bare `break` never come from it.
  The entry point's own `return` stays `flat_return`, which means `leave $root`, and `e.root` is `none` on its trees.
- **`emit(m, index, t)` legalizes, `emit_entry_point` does not.** The second refuses a structured tree with `not-core`; call `check::legalize` first.
- **The inliner never hoists.** A call's block stands where the call stood, and a value read twice (a splat, the middle of a chain) is bound where it FIRST stands.
  So a local may be declared inside a block expression and read behind it; the legalizer moves both in front together.
- **A `continue` must not cross a `once`.** In C-like text it would end the `do … while (false)`, and in WGSL's `loop { … break; }` it would spin.
  The legalizer sets a flag and breaks instead, and `find_core_violation` refuses a tree that tries.
- **Purity is declared, never inferred.** `@pure` on a prelude function lands in `function_info::is_pure` and on every `flat_call`.
  An unmarked `@builtin` is assumed to have an effect, which costs a pin and never a wrong result.
- **The interpreter's arithmetic is not a target's.** It is exact about ORDER, which is what it is for; `normalize` takes its root by iteration.
- **A flat tree has no splat and no object.** A splat is one `flat_member` per field, over a temporary local when its value is no local.
  A returned object is a `flat_construct` in FIELD order.
- **A check diagnostic is not an `sgl::diagnostic`.** It is a `located_diagnostic`: a module has several files, so it names one, and it carries a detail.
- **An emit error is no diagnostic.** It is an `emit::error`: a kind, the symbol it is about, and a detail; it has no span yet.
- **Addresses are positions.** Member i of an edge struct is location i, counted over the members without `@position`.
  What a struct is — vertex input, stage link, render targets — comes from where it stands in the signature, not from its attribute.
- **A name is renamed per target where the target reserves it**: `target` is `target_` in WGSL only, and an entry point named `main` is `main_` in MSL.
  `emitted_text::entry_point` is the name the text declares.
- **An `@inline binding`** is `register(b0, space9)`, `[[vk::push_constant]]`, `@group(3) @binding(0)`.
  **Any other binding is a group**, numbered by its place in the entry point's list, and refused in MSL.
  Its resource at `slot` is `register(<class>slot, spaceN)` in dx12, `[[vk::binding(slot, N)]]` in vulkan, `@group(N) @binding(slot)` in WGSL.
  An entry point lists at most three groups besides its `@inline` binding, as sg binds; a fourth is `too-many-groups` on every target.
  MSL has no globals, so there it is the entry point's parameter `constant T& name [[buffer(4)]]`.
  Its members must land on the same offsets in HLSL, WGSL and MSL, so `{float; float3}` is `layout-mismatch`.
  **So is `{float3; float}`**: MSL's `float3` is 16 bytes, so nothing fits into its tail, and `{float3; mat4; float}` is fine.
- **`compile_to_text` drops warnings.** It gives the text or the errors; a caller that wants warnings runs the phases itself.
- **`prelude/builtins.sgl` is GENERATED and committed; never edit it.** A hand edit fails `dev.py check` (`sgl-prelude`) and a library test.
  `prelude/core.sgl` is the hand-written half, embedded at CONFIGURE time: editing it re-runs CMake, and a test pins the embedded text to the file.
- **`*.sgl` is `eol=lf` in `.gitattributes`.** The generated text is compared byte for byte, and a diagnostic is a byte offset.
- **A new target is a `dialect`**: one file under `emit/impl/` and one case in `dialect_of`; the walk over the flat tree is shared (`impl/text_writer.cc`).
- **Every `sgl` fence under `docs/spec/` is a test** (`tests/spec/spec-examples-test.cc`): `sgl` must parse cleanly, `sgl error` must report the kind its lead names, `sgl sketch` is unchecked.
