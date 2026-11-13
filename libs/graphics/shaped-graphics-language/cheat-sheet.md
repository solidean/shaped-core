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
r.value().footprint                        // check::slot_footprint per TOUCHED slot: host_name, view (constants/read_only/storage), reads, writes
                                           // a member never named is absent — sg skips its barrier; see spec/bindings.md "Footprint"
r.value().warnings                         // cc::vector<cc::string>, one per warning: `cube.sgl:7:14: warning: unroll-too-long: ...`
                                           // notes on the lines under it, no final line end; the whole source's, so every entry point's alike
r.value().lines  .line_files               // emit::line_origin per body line: emitted (1-based), at {file, line}, inlined_from
                                           // innermost first; the prelude's frames left out, so a builtin maps to its caller (EMIT-181)
r.error()                                  // one line per diagnostic: `cube.sgl:12:5: error: unknown-name: foo`; errors alone, never a warning
sgl::compile_all_to_text({.source = text, .source_name = "cube.sgl", .targets = sgl::emit::all_targets()});
                                           // -> cc::result<cc::vector<entry_text>, cc::string>: every entry point on every target
                                           // from ONE check; compile_to_text per entry point and target checks that many times
                                           // one inside the prelude names `builtins.sgl` or `core.sgl`
                                           // a missing entry point names the ones the source holds; a wrong stage says both
sgl::text_request                          // source, source_name ("<sgl>"), library, entry_point, stage (none = any), target, options, run_tests,
                                           // line_directives: `#line n "file"` in HLSL and MSL (EMIT-182); slib asks for them, `sgl emit` only with --line-directives
                                           // native_line_table: each operation's line in SGL's own SPIR-V and DXIL (EMIT-257); opt-in, since it carries
                                           // the sources; off writes the same bytes; `vulkan_config` / `dx12_config::native_line_tables` ask for it
                                           // the entry point is found by NAME; source_name is never opened
                                           // run_tests: the source's own tests run, and one that fails is an error
                                           // options: check::option_value{name, value} per `@option const` set, value as SGL spells it:
                                           // `16`, `-3`, `0.5`, `true`, `.rgba16_float`; the rest keep their defaults, tests always run at them
                                           // a used module's option is named `common.taps`; a name no option has, a value of
                                           // another type, or a name given twice: `invalid-option`
r.value().options                          // the options the entry point reaches, by name: each set of their values is one text
r.value().library_files                    // the names of the library files the source reached: what an edit of it depends on

#include <shaped-graphics-language/driver/library_file.hh>
sgl::library_file{.name = "modules/view.sgl", .source = text}
                                           // a file the source may `use` a module of; every request takes a span of them
                                           // grouped by their `module` line; one without it, or named like the source, is left out
sgl::is_same_path(a, b)                    // one path? a path or a file:// uri, either separator, `.`/`..` read lexically,
                                           // any case on Windows; nothing is opened, so relative vs absolute differ

#include <shaped-graphics-language/driver/test_source.hh>
auto const t = sgl::test_source(text, "colors.sgl", library);   // library optional
                                           // -> tested_source { errors, warnings, test_count, tests_run, tests_passed,
                                           // tests_expecting_diagnostics, entry_points }; t.is_clean(): nothing to report

#include <shaped-graphics-language/driver/describe.hh>
auto const d = sgl::describe({.source = text, .source_name = "cube.sgl"});
                                           // -> cc::result<module_description, cc::string>: what the host side is generated from
sgl::describe({.library = files, .module = "view"});   // module `view` instead of a source: its bindings and structs, no entry point
                                           // a binding of another module is named `view.frame`; a memory struct of one carries `module`
d.value().bindings                         // name, is_inline, members (constant: offset + size; buffer: slot + host_name `work.values`; a binding array: `count` slots from `slot`), block_size
                                           // texture / image / sampler members also carry the sg enum values of their binding:
                                           // texture_dimension, sample_type, image_format + access, sampler_type, static_sampler
d.value().structs                          // the @vertex / @pixel structs: name, edge, members with their location
d.value().entry_points                     // name, stage, workgroup, bindings (the list as written, @workgroup ones left out), footprint,
                                           // samplers: the file-scope samplers its code reaches
d.value().samplers                         // the file-scope samplers: name, index (declaration order), sampler_type, settings, shape
d.value().options                          // every `@option const`: name, type (`bool`, `int`, `float`, `uint`, an enum's name), value
                                           // as described, and `follows`: the options its default is computed from (CHK-519)
                                           // entry points, pipelines and bindings each carry the `options` they reach or name
@expect(footprint = "work: read, work.values: read write")   // on an entry point: pins its footprint (CHK-267), any order
d.value().pipelines                        // name, stages, layout, vertex_input, target_set, targets, settings, open (the `.host` paths),
                                           // samplers: the file-scope ones any stage reaches, which its one layout holds
                                           // bindings and structs carry `shape`: check::structural_hash of their members,
                                           // 32 hex digits; the type's own name is not in it. What a hot reload compares.
                                           // types are SGL spellings (`float3`, `mat4`); mapping them to a host is the reader's job
                                           // only the file's own declarations, and only what the emitter would build

#include <shaped-graphics-language/driver/prelude.hh>
sgl::prelude_files()                       // -> cc::span<prelude_file const> { name, source }, in module order:
                                           // "builtins.sgl": GENERATED in memory from the builtin registry, never read from disk
                                           // "core.sgl": the hand-written prelude/core.sgl as it was when the library was built
sgl::parsed_prelude()                      // -> cc::span<parsed_prelude_file const> { file, ast }: the same files, parsed ONCE per
                                           // process and shared by every thread; point a module_file at it rather than parsing again
sgl::prelude_file_of(path)                 // -> i32: which prelude file a path or file:// uri is, else -1; only a file in
                                           // a shaped-graphics-language/prelude/ dir counts: shaders/prelude/core.sgl is -1
                                           // a driver checks such a source IN that file's place, never behind a 2nd prelude
sgl::shipped_library()                     // -> cc::span<library_file const>: the modules SGL ships ("accel.sgl", module `accel`),
                                           // which join a library wherever a file has `use accel`; no module dir needed
                                           // its records are `@target_layout` (CHK-522): fields in each target's own order
sgl::check::check_library(prelude, library)  // -> optional<checked_prelude>: every module once, one layer for every
                                           // program reaching any of them; none where a module reports anything,
                                           // a file's parse error included; the chain below, from scratch
sgl::check::library_chain(library, late_files = {})  // -> vector<chained_module> { name, files, has_options }:
                                           // dependencies first; the modules of late_files (being edited), then those
                                           // with options, as late as allowed; empty where the library makes no chain
sgl::check::check_next_module(base, library, name, options = {})  // -> optional<checked_prelude>: the next link,
                                           // over the prelude's layer or the link before; a program reaching only its
                                           // modules continues it; `options`: the values for name's own options alone
sgl::check::options_of_module(options, name)  // -> vector<option_value>: those named `name.x`, in the order given
sgl::check::relocate_link(link, old_base, new_base) // -> optional<checked_prelude>: `link`, checked over `old_base`,
                                           // moved onto `new_base` without checking it; the bases are chains of the same
                                           // modules and files, any of them changed; none where a fresh check could
                                           // differ: check it then
layer.is_relocated()                       // -> bool: relocate_link or load_link_artifact made it
sgl::check::layer_difference(a, b)         // -> string: the field path where two layers' states first differ, strings
                                           // by their text, "" if none; what a test names a relocation's mistake by
sgl::check::may_continue_from(layer, library, program)  // -> bool: a layer of its very modules, or the library's
m.is_reached_file(file)                    // -> bool: false for a module the program does not use, which a layer of the
                                           // whole library holds; ASK IT in any pass over every symbol or pipeline
m.reached_library_files                    // what the program reaches; library_files is what was checked, in file order
sgl::check::layer_serves_options(layer, options)  // -> bool: the values given the layer's modules' options are the
                                           // very ones it was checked with; a program's own options never matter

#include <shaped-graphics-language/driver/shared_library.hh>
sgl::shared_library_of(library, source_name)  // -> shared_ptr<shared_library const>: the module files parsed ONCE per
                                           // process, keyed on every name and source; null where none declares a module
sgl::layer_of(*shared, program, options = {})  // -> shared_library::link_ptr: link->layer is what to continue from,
                                           // made once and kept in `shared`; null where none serves. HOLD THE LINK as
                                           // long as the module checked behind it: it keeps the files below alive
                                           // a module's option set: a chain with its values, the links before shared
                                           // its chain's links are shared with every library holding the same modules
                                           // before them, so an edit checks its module and those after it alone
sgl::module_artifact_keys(library, source_name)  // -> vector<string>: one per module; what a host keeping artifacts
                                           // fetches, ASYNCHRONOUSLY and before the compile, which reads nothing itself
sgl::keep_module_artifacts(true)           // off by default; off again drops what was handed in or made
sgl::offer_module_artifacts(artifacts)     // span<module_artifact{key, bytes}>: a compile loads a link from them where it
                                           // would check it, if it loads over the modules before it as they stand
sgl::take_module_artifacts()               // -> vector<module_artifact>: made since the last call, for the host to keep

#include <shaped-graphics-language/check/artifact.hh>
sgl::check::save_artifact(layer, key)      // -> optional<vector<byte>>: the prelude's layer or one of modules as bytes
sgl::check::load_modules_artifact(bytes, key, prelude, library, program, registry)  // -> optional<checked_prelude>:
                                           // the layer of the modules `program` reaches, planned as check_modules plans it
sgl::check::load_artifact(bytes, key, prelude_files, registry)  // -> optional<checked_prelude>: none for another key,
                                           // other prelude sources, or bytes cut short or changed: a miss, never an error
sgl::check::save_link_artifact(link, base, key)  // -> optional<vector<byte>>: a link of a chain, at its defaults, as
                                           // its own part and where its chain's parts lay
sgl::check::load_link_artifact(bytes, key, base, library, module)  // -> optional<checked_prelude>: that link over `base`
                                           // as it stands, moved past what changed below it; a miss where it may not

#include <shaped-graphics-language/source/format_diagnostic.hh>
sgl::line_column_of(source, offset)        // -> sgl::line_column { line, column }, both 1-based, columns in bytes
                                           // lines end at `\n`, `\r\n` and a bare `\r`, as the line tree ends them
sgl::format_diagnostic(name, source, d, detail = {})   // `a.sgl:2:2: error: unknown-name: foo`; warning / error from d.level
                                                       // no detail: the kind's summary_of stands in its place
sgl::format_note(name, source, where, message)         // `a.sgl:3:1: note: declared here`, the line after a diagnostic
```

What an editor asks — the library answers it, and a language server only translates:

```cpp
#include <shaped-graphics-language/driver/classify.hh>
sgl::classify(file, ast, {.module = &m, .file_index = f})
                                           // -> vector<classified_span { where, cls, is_declaration, is_from_prelude }>
                                           // every token that has a class, in order, never overlapping; a fused number is ONE span
                                           // without .module: the syntax and the file's declarations; every USE is `name`
sgl::token_class                           // keyword control number string comment doc_comment op attribute struct_ enum_ type
                                           // function method property field binding binding_member enum_case constant pipeline
                                           // parameter argument local mutable_local self_ name; to_string(c) -> "mutable-local"

#include <shaped-graphics-language/driver/unannotated_bindings.hh>
sgl::unannotated_bindings(pf, ast, m, f)   // -> vector<unannotated_binding { name, type, is_type_named }>: each `let x = …`
                                           // that writes no type; is_type_named: `let v = vec3(…)`, where a hint repeats the line
                                           // left out: a pattern that is no name, a body the check never reached, the error type
#include <shaped-graphics-language/driver/inferred_results.hh>
sgl::inferred_results(pf, ast, m, f)       // -> vector<inferred_result { arrow, type, is_writable }>: each `=> value` without `->`
#include <shaped-graphics-language/driver/references.hh>
sgl::reference_at(files, m, f, offset)     // -> optional<reference { where, cls, declaring_file, declared, signature, doc }>
                                           // a use or a declaration; an offset right AFTER a name still names it
                                           // signature in SGL's words: `fun mix(a: float, b: float, t: float) -> float`, `let d : float`
                                           // doc: the `///` lines above the declaration; declaring_file -1 for a swizzle or a length
sgl::completions_at(files, m, f, offset)   // -> vector<completion { label, cls, detail }>, filtered by the word ending at offset
                                           // after `x.`: x's members; else locals in scope, parameters, declarations, keywords
                                           // `x.` that does not parse yet: x is read as a name; one entry per overloaded name
                                           // reference::overloads: the rest of a function's overload set, as signatures
sgl::occurrences_at(files, m, f, offset)   // -> vector<occurrence { file, where, is_declaration }>: what rename edits
                                           // empty for a swizzle or self; another overload of the name is another function
sgl::signature_help_at(files, m, f, offset) // -> optional<signature_help { signatures, active_signature, active_parameter }>
                                           // reads the tokens, so a half-written call that does not parse still answers
sgl::is_keyword(word)                      // -> bool
#include <shaped-graphics-language/driver/format.hh>
#include <shaped-graphics-language/driver/outline.hh>
sgl::outline_of(pf, ast)                   // -> vector<outline_entry { name, kind, where, name_span, detail, children }>; tree only
sgl::folds_of(pf)                          // -> vector<fold_range { first_line, last_line, is_comment }>: the line tree's blocks
sgl::format_source(source)                 // -> formatted_source { text, skipped, unformatted_lines }
                                           // spacing within lines, `name: type`, trailing comments aligned within 16 columns
                                           // indentation: 4 spaces a level; comment- and string-owned lines move along
                                           // skipped: the block and siblings around each syntax error, left as written
                                           // the output parses to the same tree, checked; unformatted_lines are formatter bugs
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
`comparison_chain` `cast` `membership` `ascription` `range` `lambda` `case_expr` `if_expr` `loop_expr` `return_expr` `yield_expr`
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
- `if_expr` is `if c => a else b` on one line, as `if_branch`es with the closing `else` last; its `else` is an `op` leaf of its form.

Statements: `invalid_stmt` `let_stmt` `assign_stmt` `if_stmt` (the whole chain, as `if_branch`es) `for_stmt` `while_stmt`
`assert_stmt` `print_stmt` `decl_stmt` `expr_stmt`.

Declarations: `invalid_decl` `module_decl` `use_decl` `require_decl` `fun_decl` `struct_decl` `enum_decl` `type_decl` `const_decl`
`binding_decl` `sampler_decl` `notation_decl` `test_decl`, and the member lines `field_decl` `property_decl` `enum_case_decl`.

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
f.writes_name(language::hlsl, "asuint")    // true for `x.bits`: its custom writer lists the name in `write.hlsl_names`
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
sgl::check::check(prelude_files, library, program, registry, options)   // library: the files `use` may reach;
                                           // options: an option_value per `@option const` the program sets; both optional
                                           // prelude_files: cc::span<module_file const>, from sgl::parsed_prelude() or a test's own
                                           // file i is prelude file i, and the program is the LAST file; never concatenated
                                           // carried: let / let mut, assignment and `op=`, if chains, while, for over `a ..< b`, loop with
                                           // break / break value / continue, and / or / not, comparison chains, int literals, print,
                                           // and calls of the program's own functions, overloads included, which are INLINED
m.prelude_file_count()                     // -> i32: every file but the last; a symbol of a file below it is the prelude's
m.symbols                                  // every top-level fun / struct / binding: file, declaration, kind, state, name,
                                           // intrinsic (builtin_id) / intrinsic_type (builtin_type_id), operator_spelling, type, info
m.builtins                                 // the registry those ids are positions in; m.builtin_type_of(type_id) / m.builtin_function(id)
                                           // -> the record, or null
m.types  m.members                         // canonical types; types[0] is the error type, types[1] (void_type) what a fun without
                                           // `-> T` returns; fields and binding members
m.functions  m.parameters  m.binding_lists // signatures; symbol::info is the position in functions / bindings
m.bindings                                 // binding_info { symbol, is_inline, members }
m.samplers                                 // sampler_state per `sampler name:` block; member_info::static_sampler indexes a binding's,
                                           // and symbol::info a file-scope one's (symbol_kind::sampler, a flat_file_sampler in a tree)
                                           // resource types (texture / image / sampler) are interned like buffers; name_of spells them
                                           // `out image_2d[.rgba8_unorm]`; check/resources.hh holds the shapes and the image formats
m.files[f].type_at(expr_id)                // side table: type_id, none for what nothing checked
m.files[f].target_at(expr_id)              // side table: { kind, symbol, index } — local / parameter / symbol / overload /
                                           // constructor / field / binding_member / enum_case / receiver (`self`)
m.files[f].call_at(expr_id)                // side table: a position in m.call_records for a call that resolved, -1 otherwise
m.call_records[i]                          // call_record { callee, written, slots }: the arguments IN THE ORDER WRITTEN, and per
                                           // parameter which one fills it, -1 for its default; what the flat tree is written from
m.near_misses                              // near_miss { file, call, candidate, reason, argument, parameter } per candidate of a call
                                           // that matched nothing: the data a later "did you mean" is written from
m.entry_points                             // flat_entry_point per SOUND entry point, in the STRUCTURED form; what an emitter reads
                                           // sound means: its body and the body of every function it reaches reported no error
m.diagnostics                              // located_diagnostic { what, file, detail, notes }, in the order they were found; a related_note
                                           // { file, where, message } is a second place a diagnostic points at
d.fixes                                    // suggested_fix { title, edits }: edits of the diagnostic's own file that fix it,
                                           // which `sgl lsp` offers as quick fixes; empty where the checker knows none
m.at(symbol_id)  m.at(type_id)  m.at(range)  m.name_of(type_id)   // name_of gives "<error>" for the error type

#include <shaped-graphics-language/check/flat.hh>
e.entry_stage  e.name  e.input  e.result  e.bindings   // stage, the name as written, edge structs, the LISTED bindings
e.features                                 // check::feature_set: what a device needs for it, from the bindings it lists
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
sgl::check::is_core(m, e)                  // the definition of the core form
sgl::check::find_core_violation(m, e)      // -> cc::optional<core_violation { reason, stmt, expr }>: the FIRST offending node
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
o.status                                   // ok, out_of_fuel, fell_off_the_end, type_error, uninitialized_read,
                                           // assertion_failed: never asserts
o.result  o.trace  o.detail                // value { type, leaves }; trace = every print, and every call with an effect
o.failures  o.checks_run                   // check_failure { site, values, is_evaluated, loop_values } per false check;
                                           // {.run_checks = false} skips checks as the core form does, {.max_failures = 8}
o.sites                                    // site_tally { passed, failed } parallel to e.check_sites, past max_failures too;
                                           // both zero: the run never reached that check
{.stop = &flag}                            // run_limits: a raised cc::atomic<bool> ends the run as `stopped`, read every
                                           // 4096 steps; how an editor abandons a test whose document changed
o == other                                 // status, result and trace; NOT the detail
sgl::check::zero_value(m, type)  sgl::check::leaf_count_of(m, type)   // a value is its scalars in field order; mat4 is 16
sgl::check::scalar::of(0.5f)  .as_float()  .as_int()  .as_bool()      // equality is on the BITS
sgl::check::dump(o)                        // `ok 1.5 | print 1 | print true`
sgl::check::is_constant(m, e, id)          // literals, and pure builtins, constructions and members of them alone (CHK-310)
sgl::check::evaluate_constant(m, e, id)    // -> outcome: `ok` with .result, or `program_error` where a call has no value
sgl::check::driver_bindings{.groups = {{.name = "frame", .members = {{.name = "exposure", .bytes = b}}}}, .acceleration_pool = p}
                                           // bound BY NAME (EVAL-94): a value is its scalars, 4 bytes each, NEVER padded;
                                           // a buffer its elements so; .mutable_bytes for a mut buffer; .acceleration_root
{.name = "albedo", .textures = {{.width = 4, .height = 4, .levels = 3, .texels = t}}}   // EVAL-120: the texels the
                                           // shader reads, 4 bytes a scalar, level, layer, slice, row, texel; an image's
                                           // .mutable_texels come back; .samplers = {sampler_state{...}} for a dynamic one
                                           // a texture bound nothing is empty: size 0, and a read of it a program-error
                                           // filtering is exact f32 (EVAL-122): compare between texel centres to ~8 bits
{.lane_count = 32}                         // run_inputs: the run is 32 lanes of one subgroup, run one after another;
                                           // subgroup ops and derivatives exchange between them (EVAL-140 to EVAL-145)
{.lane_parameters = {p0, p1, p2, p3}}      // run_inputs: each lane's own stage struct, a quad's four pixels; o.lane_results
                                           // holds every lane's result, lane order (EVAL-148)
sgl::check::resolve_inputs(m, e, bound)    // -> cc::result<run_inputs>; left out: zero, empty, root 0 (the empty TLAS)
sgl::check::write_back(m, o, bound)        // what the run stored, into each .mutable_bytes and image .mutable_texels

#include <shaped-graphics-language/test/run_tests.hh>
m.tests  m.test_units                      // test_info { symbol, file, where, scope_path, comment, unit } and its flat tree
sgl::test::run_tests(m, files, {.file = f})  // -> vector<test_result { test, status, failures, checks_run }>; files are the
                                           // module_files m was checked from, since a report quotes the source
sgl::test::run_test(m, files, t, limits, bound)  // ONE test, m.tests[t]: what a caller that stops between tests runs;
                                           // a test that expects diagnostics: judged_by_diagnostics; one that did not check: not_run
{.bindings = bound}                        // test_options: what `test {frame}:` lists gets its values here (CHK-333);
                                           // bytes that do not fit: invalid_bindings; a test may state texels itself
                                           // a test reaching subgroup ops runs at 4 to 128 lanes, derivatives at 4 (EVAL-140)
scene.albedo.test_texel(int2(x, y), v)     // SGL, in a test alone: states a texel; one the driver bound none of grows (EVAL-127)
scene.smp.test_state(scene.nearest)        // SGL, in a test alone: a dynamic sampler takes another's state (EVAL-128)
test_lane()  test_lane_count()             // SGL, in a test alone: this lane, and how many the subgroup has (EVAL-141)
r.sites                                    // site_mark { file, where, is_assert, passed, failed } per check and assert of its tree
                                           // an assert inlined from a helper names the helper's file
sgl::test::diagnostic_of(m, r)             // `test-failed` at the test, one related note per narrowed part:
                                           // "`s.z > 0.6` is 0.5 > 0.6, with i = 2"
```

## The `sgl` tool (`tools/sgl/`, a nexus binary of COMMANDs; built under `SC_BUILD_TOOLS`)

```bash
uv run dev.py run sgl -- emit shader.sgl --entry main_cs --target spirv --spirv out.spv   # SGL's own SPIR-V, compute alone so far (EMIT-232); prints its ir::program
uv run dev.py run sgl -- emit shader.sgl --entry main_cs --target dxil --dxil out.dxil     # SGL's own DXIL, the same (EMIT-234); `dxc -dumpbin out.dxil` reads it
uv run dev.py run sgl -- emit shader.sgl --entry main_ps --target wgsl   # the text and its warnings on stderr, or the diagnostics and exit 2
uv run dev.py run sgl -- emit shader.sgl --entry main_cs --option tile=16 # an option set for this compile; `describe` takes it too
uv run dev.py run sgl -- emit shader.sgl --entry main_cs --undefined report --diagnostics  # the guards of LEGAL-65, LEGAL-66 and LEGAL-69 (workgroup memory read unstored); abort: no report; define: WGSL's values
uv run dev.py run sgl -- emit shader.sgl --entry main_cs --target wgsl --weaken-float     # a @float WGSL cannot keep, at .fast with a warning
uv run dev.py run sgl -- test a.sgl b.sgl                                # the tests of each file; exit 2 when one fails
#   emit, test and describe take --module-dir DIR, repeatable: the .sgl files directly in it are modules to `use`
uv run dev.py run sgl -- describe --module view --module-dir shaders     # a module of the directories, as its package entry reads it
uv run dev.py run sgl --preset release-clang -- bench list.txt           # `path<TAB>entry` lines: each one's fastest compile of 10, per target, in-process
uv run dev.py run sgl -- prelude [--check <path> | --write <path>]       # the generated builtins.sgl; --check exits 2 on a difference
uv run dev.py run sgl -- describe shader.sgl                             # sgl::describe as JSON: what slib's generator reads;
                                                                         # each entry point and pipeline carries its sg `features`
sgl lsp                                                                  # the language server over stdio; the VS Code extension starts it
uv run dev.py run sgl -- format [--check] a.sgl b.sgl                    # SGL's spacing in place; --check exits 2 where a file would change
uv run dev.py test sgl                                                   # the language server's tests, carried by the binary (docs/lsp.md)
uv run dev.py check sgl-prelude [--fix]                                  # the gate over prelude/builtins.sgl
uv run dev.py check sgl-format [--fix]                                   # every tracked .sgl is what `sgl format` writes
uv run dev.py check sgl-native-size                                      # native SPIR-V/DXIL size against tools/native-size-baseline.json
```

## Emitting

```cpp
#include <shaped-graphics-language/emit/emit.hh>
sgl::emit::target                          // hlsl_dx12, hlsl_vulkan, wgsl, msl: a text format PLUS a backend's addressing rules
                                           // msl's groups are argument buffers; sg's tier-1 tests run it on a Metal GPU
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
sgl::emit::is_reserved(t, "target")        // true for wgsl only; msl also reserves every name Metal's headers declare, and `main`
```

## Diagnostics

```cpp
#include <shaped-graphics-language/source/diagnostic.hh>
sgl::diagnostic            // { kind, level, where }
sgl::to_string(kind)       // -> the stable kebab-case name: "undelimited-string"
sgl::summary_of(kind)      // -> one sentence for a reader: "a bracket that is opened and never closed"
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

## The language: places, function values, generics

```sgl sketch
@option const tile = 8                   // the host sets it per compile, and this is its default (CHK-353)
@option const strength = 0.25            // any scalar: a float is set by its literal and holds the nearest float
@option const spread = strength * 4.0    // a default naming another option FOLLOWS it unless set itself (CHK-519)
type color = float3                      // an alias: its name stands for the type in every type position (CHK-14)
enum stage_bit:
    vertex = 1 << 0                      // a case value is any int computed before anything runs (CHK-146)
f(mut v.zx)                              // a swizzle of distinct letters is a mut argument: the body writes those lanes (CHK-352)
@compute(tile, tile) fun blur(...)       // an option stands where a const stands: a workgroup size, an array length, an image format
                                         // a used module's option is set as `common.taps`, apart from the program's `taps` (CHK-354)
texel[.rg16_float]                       // the type an image of that format loads and stores: float2; texel[out_format] per compile (CHK-390)
img[xy] = v as texel[out_format]         // a store into a host-picked format: .x / .xy of a wider v, a narrower one filled from (0, 0, 0, 1)
                                         // the ONLY width change: `float4 as float2` is no conversion, and no kind changes (CHK-391, CHK-392)
if tile > 8: ...                         // a branch on a constant keeps the side it takes, in text, footprint and verdicts (CHK-356)
fun f(x: float, .fast: bool)             // ...and a literal argument is a constant, so `f(x, fast = false)` sheds its fast side
const if fast: ... else: ...             // the same, but a condition no constant decides once inlined is ERROR invalid-constant-argument (CHK-425)
                                         // both sides are still checked; it decides what is written, never what is checked
const if target_api == .vulkan: ...      // the compile's own API (.none .dx12 .vulkan .metal .webgpu) and target_language (.sgl .hlsl .wgsl .msl)
const if has(.subgroups): ...            // the compile's device; the branch it takes uses the feature without a `require` (CHK-426)
const if T == float: ...                 // in a generic: each instance checks only the branch its arguments leave (CHK-429, CHK-430)
                                         // a test sees .none, .sgl and no feature; a const holding any of them is ERROR, a function may return them
fun f(x: float) -> float:                // a body in the target's own text: a run of "hlsl / "wgsl / "msl, or one per const if branch (CHK-427)
    "hlsl                                //   $x is the argument, $x.y its member, $result the value; the text is unchecked
        $result = rsqrt($x);             //   a target with no alternative and no SGL branch is ERROR invalid-target-text
    "                                    //   a resource parameter is its member, `$b[$i]`; `#line` names the text's own lines
@pure @subgroup fun lane_sum(x: float) -> float: ...  // what a text body does (CHK-431): @pure for no effect and a result as uniform
                                         //   as its arguments; @barrier @subgroup @derivatives need uniform control flow
struct hit:                              // a text type (CHK-432): the target's own type where a branch spells it,
    const if target_api == .dx12:        //   its SGL fields elsewhere; local-only, and no field or constructor where
        "hlsl                            //   it is text, so its fields are read in the SGL branches of const if bodies
            dx::HitObject
        "
    else:
        t: float
"hlsl                                    // at file scope: a preamble, written once ahead of an entry point that writes
    float helper(float x);               //   a text body of this file in that language (CHK-428); no placeholders
"
@unroll                                  // on a `for`: the body once per index, the index a literal in each copy, within 256 copies (CHK-389)
for i in 0 ..< taps: ...                 // nothing of it runs as a loop, no text holds a hint, and the bounds are int constants
                                         // past 256 copies, a nest's counted together, it stays a loop: WARNING unroll-too-long
                                         // ...and so does an inner one whose bound reads its index, under the same warning
                                         // a rule judging a constant judges each index: `1u << (i as uint)` past 31 is refused
@max_iterations(64)                      // on a `while` or a `for`: ends it after 64 iterations at the latest, in every compile
while t < far: ...                       //   (CHK-528); a bound kept, not a promise, so bwd_diff walks it back over a tape

@float(.specials) fun clean(v: float) -> float:   // the least float guarantee a body needs (CHK-435): .fast (default),
    return if v != v => 0.0 else v       //   .specials, .precise, .ieee; an entry point keeps the strictest it reaches
                                         // dx12 keeps all, msl up to .precise, vulkan and wgsl refuse above .fast (EMIT-185)
                                         // `v != v` at .fast: WARNING nan-test-at-fast; is_nan tests the bits, fine anywhere

fun bump(c: mut counter, by: float):     // a mut parameter: the caller's place (CHK-315)
    c.total += by
bump(mut c, 2.0)                         // the call marks it; exact type, indices evaluated once (CHK-316)

fun apply(f: (float) -> float, x: float) -> float => f(x)   // a function type: parameters only (CHK-317)
apply(halve, 3.0)                        // a function's name, whose signature is the type's exactly
apply(x => x * k, 2.0)                   // an arrow lambda, seeing `k`; inlined where `f` is called (CHK-319)

fun twice[A](x: A, f: (A) -> A) -> A => f(f(x))   // the body is checked per instance, its names where written (CHK-338)
fun sum[T](a: T, b: T) => a + b          // so a body may use what each argument type has; a test that calls it checks it
twice(3, x => x + 1)                     // A deduced from arguments, a lambda's result, or where the call stands (CHK-340)
converted[int](2.75)                     // generic arguments stated by position or `[.To = int]`, the rest deduced (CHK-340)
fun zero_of[T]() -> T => T(0)             // a call of T is a call of what it stands for in each instance
fun scaled_by[n: int](x: float) => x * float(n)   // a value parameter, a constant in each instance; call scaled_by[3](x)
fun combined[m: mode](a: float) => …    // an enum one: combined[mode.add](x); a type position takes `op[.add]`
fun copy_texel[f: pixel_format](img: mut image_2d[f], a: int2, b: int2)   // an image's format, deduced from the image handed over
struct box[T = float, n: int = 2]:      // defaults bind what nothing else says; box[int] is box[int, 2]
fun max[V](a: V, b: scalar_of[V]) -> V  // scalar_of[V]: the scalar of a vector or matrix type (CHK-423)
```

## The language: ray tracing ([docs/spec/raytracing.md](docs/spec/raytracing.md))

```sgl sketch
require ray_query                                        // inline traces; raytracing_pipeline for the pipeline
binding scene:
    world: acceleration_structure[.triangles]            // or .procedural, .mixed; the argument is required (CHK-320)

let h = scene.world.trace(r)                             // -> triangle_hit; h.is_hit, h.t, h.barycentrics, h.instance_id, …
let h = scene.world.trace(r, cutout)                     // cutout: (triangle_candidate) -> hit_decision, a name or a lambda
let h = scene.world.trace(r, flags = ray_flags.force_opaque, mask = 0x01)
let h = scene.world.trace(r, cutout, flags = ray_flags.force_omm_2_state)  // micromap unknowns take their guess, never asking cutout
let r = ray(origin = o, direction = d, time = 0.5)       // moving instances and vertices stand where `time` puts them, 0 to 1 (EVAL-130, EVAL-132); emulated trace only, which dx12 and vulkan take through a package's SOFTWARE_TRACE
let p = spheres.trace(r, sphere, cutout_sphere)          // .procedural: (procedural_box) -> report[A], then the any hit
let m = both.trace(r, any_hit = a, intersection = s, procedural_any_hit = b)   // .mixed -> mixed_hit[A]: m.kind, m.triangle(), m.procedural()
is_occluded(scene.world, r)                              // the shadow ray
h.to_world(p: pos3)  h.to_world(v: vec3)  h.normal_to_world(n)  h.to_object(p)  // typed transforms
h.object_to_world  h.world_to_object                                        // affine3: a * p translates, a * v does not
report(t = t, attributes = a)  report.none()             // what an intersection returns: at most one per box

rays path_rays:                                          // a ray set: position = contribution and miss index
    surface: radiance
    occlusion: shadow
@raygen fun primary(@launch_id id: int3){b}              // @launch_id / @launch_size: int3, every ray stage (CHK-327)
@miss fun sky(p: mut radiance)                           // the payload is the one `mut` parameter (CHK-328)
@closest_hit fun shade(h: triangle_hit, p: mut radiance)
@any_hit fun cutout(c: triangle_candidate, p: mut radiance) -> hit_decision
@intersection fun sphere(b: procedural_box) -> report[sphere_attributes]   // no payload; A a struct of the program
@callable fun doubled(v: mut operand)
trace(b.world, r, path_rays.surface, mut p, flags = f)   // a trace of a ray type: raygen, miss, closest hit only (CHK-329)
maybe_reorder(hint, 4)                                   // raygen: regroup invocations by hint's low bits; SER on dx12 (SM 6.9) and vulkan, else nothing
trace(b.world, r, set.ray, mut p, reorder = true)       // raygen: SER (dx12, vulkan) traces a hit object, regroups by it, invokes; else plain
trace(…, reorder = material, reorder_bits = 4)          // by the hit and a uint hint's low bits, 1..16 (CHK-433)
let h = trace_hit(b.world, r, set.ray, mut p)           // raygen, under has(.shader_execution_reordering): no closest hit or miss yet
h.is_hit; h.t; h.instance_id; h.primitive_index         // what was hit, before its shaders run (CHK-434)
h.reorder(); h.reorder(hint, 4); h.invoke(mut p)        // regroup by it, then run its shaders on that ray type's payload
hit_group textured for path_rays:                        // one row: a record per ray type (CHK-330)
    surface = (closest_hit = shade, any_hit = cutout)
    occlusion = ()                                       // empty: accept, run no closest hit
@raytracing pipeline path:                               // payload size, attribute size and depth are DERIVED (CHK-331)
    rays = path_rays
    raygen = primary
    miss.surface = sky
    hit_groups = (textured, .host)                       // `.host` last, and then max_recursion_depth = N is required
callables ops = (doubled, negated, .host)                // the module's; tables pack in declaration order (CHK-343)
ops[i](mut v)                                            // raygen, miss, closest hit, callable (CHK-344)
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
- **`no-effect` is a warning.** A paren or juxtaposition call, a jump, a `case`, a `loop` and `invalid` have an effect; nothing else does.
  A `test` body is exempt from the AST pass's warning (AST-140): a `bool` line there is a check, which only the check pass can tell.
- **An anonymous `fun` is a lambda only in expression position.** As a statement it is a function that lost its name and reports `expected-name`.
- **`type name = …` is a type position**, like the right sides of `:`, `->` and `as`; the AST dump writes it `(type name : …)`.
- **`true` and `false` are no keywords**: the AST reads them as names, and the check pass as the `@shadowable(false)` consts of `core.sgl`.
- **`self` and `void` are reserved names**, read as `self_ref` / `void_ref`; a declaration taking one is `reserved-name` (AST-141).
- **The check pass is one demand-driven pass.** A symbol is untouched, in compilation, checked or failed, and reaching one in compilation is `dependency-cycle`.
  Compiling a function means its signature; bodies are checked after every signature is known.
- **Every body is checked ONCE, on its own**, so a broken function nobody calls still reports, and one called three times reports once.
  Inlining happens afterwards, from the side tables, and only for an entry point whose every reachable body is sound.
- **Recursion is `recursive-call`**, once per loop of calls, at the call that closes it: `a -> b -> a`.
- **Bindings are an effect.** A call needs the callee's `{…}` list inside the caller's, or it is `binding-not-listed` at the call; so an entry point lists what its shader reads.
- **`require` permits, and use sets the floor.** `require extended_image_formats` in a file, a binding or a body grants the feature, named as `sg::feature` names it.
  An entry point needs what the bindings it lists use, its stage inputs, a member it takes per sample and its stage.
  It must declare each of those by its file, a listed binding or its own body, or it is `feature-not-declared`.
  A form used without a grant is `needs-feature`, an unknown name `unknown-feature`, and a body `require` nothing needed is the WARNING `unused-require`; a file's or a binding's never is.
  WGSL refuses an entry point needing `binding_arrays`, `multisampled_array_textures`, `raytracing_pipeline`, `geometry_shader` or `tessellation_shader` as `target-lacks-feature`.
  It writes `ray_query` through the emulated trace instead (EMIT-135).
  MSL refuses one needing `geometry_shader` or `tessellation_shader` the same way.
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
  Only two non-functions of one name in one file are `duplicate-declaration`, a struct beside the functions of its name aside.
- **`and`, `or` and `not` are no functions**, and a comparison chain evaluates each inner operand once: it is bound where it first stands.
- **One call model** (CHK-69): a method, a static, a property, an extension and a struct's constructor are all functions.
  `a.foo(b)` and `foo(a, b)` collect the same candidates: the functions of that name, and those of `a`'s type scope (CHK-247).
  `a.foo` is a field where there is one; otherwise the target must be a property, and `a.foo()` must not reach one (CHK-256).
  A member body reads its receiver through `self` ALONE: a bare `radius` is no field (CHK-62), while a field's default reads earlier fields bare.
- **Arguments bind by position, then by name** (CHK-250); a positional one after a named one only in its own slot, and `.x: T` is named-only.
  A default is checked ONCE in its function's scope, and evaluated at each call that leaves it out, after every written argument (EVAL-80).
- **A literal converts where a type is expected** (CHK-81, CHK-253): `(1, 2)` or `{a = 1}` is a call of the struct's name, `1` meets a float.
  Leaving its default type is one step of a literal's chain; candidates rank by dominance over those chains, then a type-scope function wins (CHK-254).
  `0xff`, `0b1010` are integer literals like decimal ones (CHK-269); `&`, `|`, `^`, `~`, `<<`, `>>` take int, uint and their vectors, a shift's count keeps its low five bits.
  `1 / 3` is `literal-needs-type` (CHK-313): `/` and `%` over integer literals alone say nothing of int or float; an integer literal is held in 64 bits and refused only in a type that cannot hold it.
  A prelude operator over literals alone folds into one (CHK-394): `(1 + 2) * 0.5` is `1.5`, `let x: float = 2 * 3` holds, and `2 < 3` is `true`.
- **The maths builtins** are records of `register_math.cc`: trig, `exp`/`log`, `pow`, `sqrt`/`inverse_sqrt`, rounding, `sign`, `step`/`smoothstep`, `is_nan`/`is_inf`.
  `is_nan` and `is_inf` are a bit test on every target, since Vulkan, WGSL and MSL may each assume neither arises.
  They take a half too, tested as the float it widens to.
  Beside them `ddx`/`ddy`, `cross`, `distance`, `reflect`/`refract`, the integer bit functions and packing; `round` is ties to even.
  **`ddx`, `ddy` and `fwidth` are coarse**, one value per quad, on every target but metal; `ddx_fine`/`ddy_fine` are the per-pixel difference (EVAL-144).
  `fma(a, b, c)` of halves, floats and doubles rounds once on the interpreter, and a GPU may round the product first except a double's on DXIL (EVAL-146, EMIT-254).
  The interpreter computes them with `builtins/impl/soft_math.hh`, never a libm, so every host meets the same bits; a test compares them with `nearly_equal(a, b, within = …)`.
  `x.bits` and `float.from_bits(u)` are extensions in `core.sgl` over `reinterpret_as_*`, and a WGSL-indeterminate argument (`pow(-1.0, 0.5)`) is a `program-error`.
  A double's `d.bits` is a `uint2`, low word first, and `double.from_bits(u)` takes one back, with no `ulong` or `shader_int64` between (CHK-517).
- **`mat4` is the one matrix**, opaque and column-major, built from its columns: `mat4(c0, c1, c2, c3)` takes four `float4` (EVAL-99), `c3` being where the origin goes.
  `register_transforms.cc` holds it, `m * p`, `m * v`, `m * float4`, `m * m`, `determinant(m) -> float` (EVAL-98) and `transpose(m)` of a `mat4`, a `mat3` or a `mat2`, every target's own function.
  `a + b`, `a - b`, `-m`, `m * s` and `s * m` act on each element of any of the three (EVAL-109).
  A `driver_bindings` matrix is 16 floats column by column too; `sgl test` alone sees a binding's matrix as zero.
- **A bare `@compute` chooses its own size** (CHK-520): 64, 8 × 8 or 4 × 4 × 4, by the axes of `@thread_id` it reads.
  It reaches no barrier, workgroup memory, subgroup operation or group input, is dispatched by threads alone, and returns past the grid sg writes.
  A stated size it could have chosen warns `workgroup-wastes-lanes` where it idles lanes (CHK-521).
- **Stage inputs are parameters** (CHK-271): the stage struct first, then `@vertex_index i: int`, `@is_front_facing f: bool`, `@thread_id id: int3`, ….
  `flat_entry_point::input` is `none` for a vertex stage without a vertex buffer, and `stage_inputs` holds each input's local; `run_inputs::stage_inputs` gives them values.
- **Geometry and tessellation stages** (CHK-301 to CHK-307): `@geometry(max_vertices = N)` takes `tri: varyings[3]` first and `stream: mut triangle_stream[varyings]` last.
  `N` is at most 256, and `N` times the scalars of `varyings` at most 1024.
  Each stage takes what the stage before hands on first, and its stage inputs after it.
  `@tessellation_control(partitioning = …, winding = …)` takes the patch and returns a factors struct, and `@tessellation_evaluation` takes both and `@domain_location`.
  HLSL writes them; WGSL and MSL refuse by the feature, so they are tested in C++ rather than in the corpus, which emits for all four targets.
- **`T[N]` is a value like a struct** (CHK-285 to CHK-290): copied where passed, `float[3, 5]` is three arrays of five, and `xs.length` is a constant.
- **`values.length` of a buffer is a `uint` the bound buffer decides** (CHK-460), of `bytes` its bytes, and `values[i].length` that element's of a binding array.
  Metal refuses it, since a Metal buffer carries no length (EMIT-195).
  The robustness profiles bound every buffer index and `bytes` access by it, except on metal (LEGAL-66).
  `T[N].filled(v)` and a square literal `[a, b, c]` build one; `T[]` is a binding member's alone.
- **`discard` is a jump, and only a `@pixel fun` may reach it** (CHK-277): anywhere else it is `stage-not-allowed` at the `discard`.
  A path that ends in one needs no value, and the uniformity pass does not count it as an exit (CHK-284).
- **A `@workgroup binding` is memory one workgroup shares** (CHK-292 to CHK-295): listed like a binding, in no group, and left out of describe.
  Only a compute entry point lists one, all of it within 16 KiB; `workgroup_barrier()`, `storage_barrier()` and `texture_barrier()` sync it (EMIT-131).
- **`atomic[uint]`, `atomic[int]` and `atomic[float]` live only in a `mut buffer` or a `@workgroup` binding** (CHK-296), and an expression of one is only ever a builtin's argument (CHK-297).
  A float's has `load`, `store`, `exchange` and `add`: the device's own float atomic where the compile's device has `float_atomic_add` (vulkan, metal),
  and a compare-exchange loop on a uint's bits elsewhere, so no pixel stage takes its `add`.
  `compare_exchange_strong(e, d)` gives the value before, and stored exactly where that is `e`; it is a loop in WGSL and MSL (EMIT-161).
  `compare_exchange_weak(e, d)` gives `exchange_result[T]`: `old`, and `exchanged`, the only proof it stored; retry while `old == e and not exchanged`.
  Both are relaxed: publishing what a claim guards needs a barrier.
  Atomics stand in pixel, compute, raygen, miss, closest-hit and callable stages; in an any hit or an intersection, or a decision an inline trace calls, they are `stage-not-allowed` (CHK-404).
  In a pixel stage a helper's atomics store nothing and read garbage; the strong loop skips one in MSL (EMIT-162).
  A pixel stage's `compare_exchange_strong` needs `require helper_lane_query` (EMIT-163), which WebGPU lacks: there, write the weak one in a loop, or use compute.
  A test fails a weak one spuriously every other time each call meets `e`, the first time first (EVAL-100), so a missing retry loop fails its test.
- **`img[xy]` is the image's `load`, and `img[xy] = v` its `store`** (CHK-367): the check pass records the call at the subscript, and `m.texel_stores` holds each assignment's.
  `@atomic` on a `mut` image of `.r32_uint` or `.r32_sint` makes `img[xy]` an atomic, `img[xy].max(v)` (CHK-372), which the flat tree calls as the prelude's internal `texel_max`.
  `@coherent` on a `mut buffer` or image is `globallycoherent` in HLSL and `coherent(device)` in MSL (CHK-368); both features are WGSL's `target-lacks-feature`.
- **`v[i]` is a vector's lane** (CHK-395): a `flat_element` whose object is a `@swizzle` struct, read and written as an array's element is; `checker::lane_type_of` says whether a type has lanes.
- **`bytes` / `mut bytes` is raw memory a 32-bit word at a time** (CHK-388): `b.load(off)`, `load2..4`, `b.store(off, uintN)`; `off` a multiple of 4.
  HLSL `ByteAddressBuffer`, else an array of words (EMIT-155); the host binds a `*_buffer_view<cc::byte>`.
- **A binding array, `texture_2d[float4][64]`, needs `binding_arrays`** and is read by element alone (CHK-299).
  `texture_2d[float4][]` is unbounded, its length the group's, under `unbounded_binding_arrays`, as its binding's last member (CHK-490).
  Its `length` is the views its group binds, a staging group's capacity, which sg writes per dispatch or draw (CHK-491).
  The generated host field is a `cc::vector` of views, one per element the group binds.
  An index the uniformity pass proves uniform needs no mark (CHK-300); `nonuniform i` is an index that may vary, and `uniform i` the author's word that it does not.
  Unmarked, a proven varying index is `non-uniform-index` and an unproven one `unproven-index`; `uniform` on a proven varying one is `uniform-index-varies` (CHK-399, CHK-400).
- **A function takes any resource as a parameter, `b: mut buffer[float]`, of exactly its member's type** (CHK-366): `mut` is the access, never a caller's place.
  Inlining substitutes the member, so the footprint and an element's index stay the caller's; `buffer[float]` is no argument of `mut buffer[float]`, nor the reverse.
  A buffer parameter is subscripted, measured or handed on; `let u = b` and a resource result stay `unsupported-yet`.
  A helper's index is judged per call: one it reaches both ways is `mixed-index`, split in two or marked `nonuniform` (CHK-401); a needless `nonuniform` is a warning.
  A stage-link member marked `@per_draw` crosses flat and is uniform in the pixel stage; the vertex stage must return it uniform, or it is `non-uniform-per-draw` (CHK-393).
- **The uniformity pass judges the inlined entry point** (CHK-282 to CHK-284) by WGSL's rules, so no target refuses what SGL accepts.
  A barrier, or a call that takes derivatives implicitly (`sample` without `level`, `ddx`), in non-uniform control flow is `non-uniform-control-flow`.
- **`@sampler(name)` on a texture member names a sampler of the same binding, or a file-scope one** (CHK-279), which a method call then leaves out: `material.albedo.sample(uv)`.
  A member of that name hides a file-scope sampler of it.
  A call without a sampler on a texture without one is `missing-sampler`.
- **A `sampler name:` at file scope is a static sampler of the pipeline layout** (CHK-314), handed to a builtin by its name: `tex.sample(uv, name)`.
  It joins the layout of every entry point whose inlined code reaches it, at its position among the file's samplers (EMIT-133).
  An entry point reaching one at position 16 or later is `too-many-samplers`, and unreached samplers declared above it count.
- **Still `unsupported-yet`:** `mut self`, lambdas beyond `x => value`, nested functions, `use`,
  a `const` whose value is no literal, enum case or const, a `for` over anything but `a ..< b`, a `let` without a value,
  an expression statement that is no call outside a `test`, an `assert` message, and an `assert` whose condition writes.
- **A function value is never a value of any target.** A function type is a parameter's whole type alone, and a call through it inlines what was handed.
  A `let f = halve` is `unsupported-yet`, and a function type under `mut`, as an element or as a type argument is `wrong-kind-of-name`.
- **A mut parameter takes no default** (`default-not-allowed-here`), and a function with one is never a function value.
- **A generic body is checked PER INSTANCE, its names where it is written.** Its own pass judges names alone; an entry point is never generic.
  A type parameter's name hides every symbol of that name, so `A(1.0)` inside `fun f[A]` is what `A` stands for, even where a struct `A` exists.
  A generic nothing instantiates warns `generic-not-instantiated`: a `test` that calls it is how it is checked.
- **Generic structs take type and value parameters**, `struct ring[n: int, T]:` with `items: T[n]`; `ring[4, float]` names an instance.
  The constructor mirrors the parameters, `ring(float[4].filled(0.0), 0)`, and a method names them, `fun ring.capacity(self) -> int => n`.
- **Two `trace`s**: with a ray type third, `trace(world, r, set.ray, mut p)`, it is the pipeline's; otherwise it is the prelude's inline trace.
  The inline one needs `ray_query` where the entry point REACHES it (CHK-322), so a stage that never traces runs on a device without it.
- **A trace graph with a cycle is `recursive-trace`** (CHK-332): a miss or closest hit tracing its own ray type, directly or through others, is refused, never bounded.
- **A payload on HLSL is `[raypayload]` with inferred qualifiers**, unless a `.host` pipeline (or no pipeline) traces it: then every field is widest (EMIT-137).
- **Metal's pipeline traces any bound TLAS**, two in one group or across groups; transforms and `accept_and_end_search` behave as DXR's, and its GPU tests pass on an M4.
- **An arrow body without `-> T` infers its result**, and a BLOCK body without one returns `void`.
  Its body is checked as part of compiling it, so two such functions that need each other are `dependency-cycle`, not `recursive-call`.
  An overload whose parameters cannot take a call is not demanded by it, so an overload set works from inside one of its inferred members.
- **A call as a statement is `flat_eval`**: evaluated, its value dropped.
  `value;` in HLSL, `_ = value;` in WGSL, `(void)(value);` in MSL; the legalizer drops one whose value was a block, once the block has moved.
- **The error type is silent.** What did not check has `checked_module::error_type`, and nothing that meets it reports again.
- **`unsupported-yet` is never a guess.** Its detail names the construct, and the construct's type is the error type.
- **A `@builtin struct` is keyed by its name, a `@builtin fun` by its name AND its parameter types**, against the registry.
  An `@operator` function is found through its operator alone: no lookup sees its name, which is documentation and what a dump shows.
- **Adding a builtin touches ONE record**, then `uv run dev.py check sgl-prelude --fix` regenerates `prelude/builtins.sgl`.
  No emitter, interpreter or layout switch exists to extend, and a test adds `sawtooth` to a registry of its own to keep that true.
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
  **Any other binding is a group**, numbered by its place in the entry point's list; MSL passes it as an argument buffer at `[[buffer(group)]]`.
  Its resource at `slot` is `register(<class>slot, spaceN)` in dx12, `[[vk::binding(slot, N)]]` in vulkan, `@group(N) @binding(slot)` in WGSL.
  An entry point lists at most three groups besides its `@inline` binding, as sg binds; a fourth is `too-many-groups` on every target.
  MSL has no globals, so there it is the entry point's parameter `constant T& name [[buffer(4)]]`.
- **GPU memory is placed at natural alignment, the same on every target, because that is what loads fastest** (EMIT-110).
  A vector aligns to its size and a 3-vector to 16, a scalar may fill a 3-vector's tail, and a struct nested in a constant block takes whole rows.
  So `buffer[float3]` strides by 16, and the host writes it as `slib::gpu_padded<tg::vec3f, 16>`; the generated structs carry every padding field.
  WGSL writes it natively, MSL packs a 3-vector whose tail holds a value, and HLSL writes padding fields into its structs (EMIT-112).
  A struct placed by two rules is `layout-conflict`; `@no_padding` turns a gap into `padding-forbidden`; `bool` has no layout, `bool32` does.
  **`@padding(.none)` packs as C++ does**, on a binding (its block and its buffers) or on one buffer member (CHK-369).
  It is for a host's own hand-written struct, or memory something else reads too, such as a BLAS's vertices; WGSL and MSL then read through a memory form.
  **An array in memory strides by its element rounded up to its alignment** (CHK-291): a `float3` by 16, a struct by its size.
  A constant block admits only elements of whole rows, unless `@padding(.every_element) weights: float[8]` gives each element a row (EMIT-172).
  HLSL holds a 3-vector array as 4-vectors (EMIT-169), and MSL packs a struct's or a block's shared tail (EMIT-170).
  A multi-dimensional array nests the natural stride, and an array of `mat3` strides by 48; the host writes a 3-vector element as `slib::gpu_padded<T, 16>`.
  A packed buffer element's array of builtins strides by its scalars, as C++ does (EMIT-178); packed arrays of structs or arrays, and nested ones of `mat3`, still wait (EMIT-171).
  **A `mat3` in memory is three 16-byte columns, 48 bytes**: the host writes `slib::gpu_mat3`, and `tg::mat3f` under `@padding(.none)`.
  HLSL holds it as a `float4x3`, which a read narrows and a write widens (EMIT-168); it rides in no ray payload yet.
- **A `mat2` in memory is two columns back to back, 16 bytes** (EMIT-175), on a row in a constant block; the host writes `tg::mat2f`.
  HLSL's constant buffer holds it as a `float4`, rebuilt on read; a packed buffer element's is 16 bytes at 4, a packed block's waits.
- **A vertex input and a stage link carry a matrix or a 1-D array** (CHK-422), a location per column or element: `frame_0`, `frame_1`, … (EMIT-174).
  A vertex input's host struct keeps the member whole, and its layout names an attribute per piece; a pixel output takes neither.
- **`scope:` opens a block whose locals end with it** (AST-156), transparent to `break`, `yield` and `return`; it flattens into its parent.
- **`floatN(…)`, `intN(…)` and `uintN(…)` convert a scalar of another of those kinds**, as `as` would (CHK-421); `vec3` and `pos3` never do.
- **`@mesh(x, y, z, vertices = N, triangles = M)` is the mesh stage** (CHK-480 to CHK-483), under `require mesh_shader`: dx12, vulkan and metal, refused on WGSL.
  Its last parameter is `output: mut mesh_output[T]`; `output.set_counts(v, t)` in uniform control flow, then `output.vertex(i, v)` and `output.triangle(i, uint3)`.
  A `pipeline` names it as `mesh = f` in the vertex stage's place, and the host draws it with `draw_mesh_tasks`.
- **`@task(x, y, z)` launches mesh workgroups** (CHK-484 to CHK-487): its body ends in `dispatch_mesh(x, y, z, payload)`, once, in uniform control flow.
  `@task(32, meshes = 64)` states the most one workgroup launches, which a polyfilled draw holds slots for.
  The mesh stage takes `payload`'s struct as a parameter before its output, and the pipeline names both: `task = t`, `mesh = m`.
- **`mesh_output[T, F]` writes `F` per triangle** with `output.primitive(i, f)` (CHK-488), and the pixel stage takes it as a second struct: `fun ps(c: T, f: F)` (CHK-489).
- **`half`, `short`, `ushort` and their vectors are the 16-bit families** (CHK-381): `require shader_f16` or `shader_int16`, and WGSL has no short.
  HLSL spells a half `float16_t`, never `half`, so slib's SGL edge hands DXC `-enable-16bit-types` on every compile; a 16-bit literal is a construction, `half(0.5)`.
  A 16-bit value aligns to its size, a `half3` to 8, and crosses no stage edge and sits in no `@inline` binding (CHK-387).
  A buffer's element is whole 4-byte words: `buffer[half2]`, never `buffer[half]`.
- **`long`, `ulong`, `double` and their vectors are the 64-bit families** (CHK-450): `require shader_int64` or `shader_f64`; WGSL has neither, MSL no double.
  Literals: `5i64`, `5u64`, `0.1f64`, or an unsuffixed literal where the type is asked; a `ulong` literal stops at 2^63 - 1, so `~0u64` is the largest.
  Arithmetic, comparisons, bits (a shift counts to 63), `abs` / `min` / `max` / `clamp` / `select`; no subgroup operation yet.
  A double has `sqrt`, `inverse_sqrt`, the exact roundings, `sign`, `saturate`, `is_nan`, `mix`, `step`, `smoothstep` and the vector geometry (CHK-458).
  Its `sqrt` is within an ULP on dx12, and GLSL.std.450's on vulkan, which Vulkan bounds only as a float's (EVAL-108, EMIT-256).
  No transcendental of a double, which no target has: `sin(x)` is `no-matching-overload`, and `sin(x as float)` is the float's.
  `as` converts within a width or within a family: `x as long as double`, never `int as double` in one step.
  A 64-bit value crosses a stage link flat, a three- or four-vector over two locations (CHK-454); dx12 and metal carry it as its 32-bit words (EMIT-196).
  None sits in an `@inline` binding, and a constant block holds no `long3` / `double4` (CHK-453); a buffer does.
  `atomic[ulong]` / `atomic[long]` in a `mut buffer` take `require shader_int64_atomics` (dx12 SM 6.6, vulkan).
  In workgroup memory they also take `shader_shared_int64_atomics` (CHK-510), which an Arc B580 lacks and an AMD Radeon iGPU has.
  A ulong's `min` and `max` alone take `shader_int64_min_max_atomics`, which `shader_int64_atomics` grants too and Metal has (CHK-511);
  MSL's give nothing back, so there they stand as statements (EMIT-218).
  **No layout is guaranteed without an annotation** (EMIT-116): the compiler may reorder members, so the host goes through the generated struct, never through offsets it assumed.
- **A warning never fails `compile_to_text`, and only a text carries one.** A failed compile's error holds its errors alone.
  slib's SGL edge logs each warning under `slib`, and `sgl emit` prints them to stderr beside the text.
- **`prelude/builtins.sgl` is GENERATED and committed; never edit it.** A hand edit fails `dev.py check` (`sgl-prelude`) and a library test.
  `prelude/core.sgl` is the hand-written half, embedded at CONFIGURE time: editing it re-runs CMake, and a test pins the embedded text to the file.
- **`*.sgl` is `eol=lf` in `.gitattributes`.** The generated text is compared byte for byte, and a diagnostic is a byte offset.
- **A new target is a `dialect`**: one file under `emit/impl/` and one case in `dialect_of`; the walk over the flat tree is shared (`impl/text_writer.cc`).
- **Every `sgl` fence under `docs/spec/` is a test** (`tests/spec/spec-examples-test.cc`): `sgl` must parse cleanly, `sgl error` must report the kind its lead names, `sgl sketch` is unchecked.
