# shaped-graphics-language cheat sheet

SGL's toolchain as a library: the syntactic half, from bytes to the form tree, the AST pass on top of it, and a tracer of the check pass and the emitters.
Namespace `sgl`.
Depends on clean-core.

The language itself is specified in [docs/spec/](docs/spec/_index.md); this sheet is the C++ API.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `sgl`, for what the *library* has to say about itself.
What the *user's source* did wrong is `sgl::diagnostic`, which is data a caller receives and never a log line.

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

## The check pass (a tracer: it carries `tests/samples/cube.sgl`, everything else is `unsupported-yet`)

```cpp
#include <shaped-graphics-language/check/check.hh>
auto const m = sgl::check::check({.file = prelude, .ast = prelude_ast}, {.file = user, .ast = user_ast});
                                           // -> sgl::check::checked_module; TOTAL; a module_file is two REFERENCES
                                           // file 0 is the prelude (prelude/prelude.sgl), file 1 the program; never concatenated
m.symbols                                  // every top-level fun / struct / binding: file, declaration, kind, state, name,
                                           // intrinsic (check::builtin), operator_spelling, type, info
m.types  m.members                         // canonical types, types[0] is the error type; fields and binding members
m.functions  m.parameters  m.binding_lists // signatures; symbol::info is the position in functions / bindings
m.bindings                                 // binding_info { symbol, is_inline, members }
m.files[f].type_at(expr_id)                // side table: type_id, none for what nothing checked
m.files[f].target_at(expr_id)              // side table: { kind, symbol, index } — local / parameter / symbol / overload /
                                           // constructor / field / binding_member
m.entry_points                             // flat_entry_point per SOUND entry point; what an emitter reads, never the AST
m.diagnostics                              // located_diagnostic { what, file, detail }, in the order they were found
m.at(symbol_id)  m.at(type_id)  m.at(range)  m.name_of(type_id)   // name_of gives "<error>" for the error type

#include <shaped-graphics-language/check/flat.hh>
e.entry_stage  e.name  e.input  e.result  e.bindings   // stage, the name as written, edge structs, the LISTED bindings
e.locals  e.exprs  e.stmts  e.body         // locals[0] is the parameter; at(id) / at(range) like the AST
sgl::check::flat_expr                      // { type, from (origin), inlined_through, node }: flat_literal, flat_local_ref,
                                           // flat_binding_member, flat_member, flat_construct, flat_call
sgl::check::flat_stmt                      // flat_let, flat_return
e.names.mint("n")                          // -> "n", then "n_1", …; EVERY name an emitter writes comes from here
e.names.reserve("main_ps")                 // -> false when taken; for a name that must not change

#include <shaped-graphics-language/check/dump.hh>
sgl::check::dump(m)                        // symbols, then entry points: (let n : vec3 = (call normalize … : vec3))
sgl::check::dump_entry_points(m)
sgl::check::dump_diagnostics(m)            // `unknown-name @1:120+4 foo`: kind, file, span, detail
```

## Emitting

```cpp
#include <shaped-graphics-language/emit/emit.hh>
sgl::emit::target                          // hlsl_dx12, hlsl_vulkan, wgsl: a text format PLUS a backend's addressing rules
sgl::emit::all_targets()                   // -> cc::span<target const>
auto const r = sgl::emit::emit(m, 0, sgl::emit::target::wgsl);   // -> emitted_text; the isize is a position in m.entry_points
                                           // ONE entry point per call: it, and exactly the structs and the binding it needs
                                           // TOTAL and deterministic; the text carries FINAL addresses, no pass numbers it later
r.has_text()  r.text                       // text is empty when there are errors
r.errors                                   // emit::error { kind, symbol, detail }; the SAME for every target
sgl::emit::to_string(target)  sgl::emit::to_string(error_kind)   // "hlsl-vulkan", "reserved-entry-point-name"
sgl::emit::dump_errors(r)                  // `unsupported a binding that is not @inline: 'scene'`, one per line

#include <shaped-graphics-language/emit/reserved_words.hh>
sgl::emit::reserved_words(t)               // -> cc::span<cc::string_view const>: keywords, predeclared types, the functions the text calls
sgl::emit::is_reserved(t, "target")        // true for wgsl only
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
- **The AST is name-free.** `build` never looks a name up, so `vec3` is a `name` and `texture2d[rgba8]` an `index` wherever they stand.
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
- **The error type is silent.** What did not check has `checked_module::error_type`, and nothing that meets it reports again.
- **`unsupported-yet` is never a guess.** Its detail names the construct, and the construct's type is the error type.
- **A `@builtin` is keyed by its NAME** (`check::builtin_of`), and an `@operator` function is found through its operator alone: no lookup sees its name.
- **An entry point with any error has no flat tree.** `m.entry_points` holds only what an emitter may read.
- **A flat tree has no splat and no object.** A splat is one `flat_member` per field, over a temporary local when its value is no local.
  A returned object is a `flat_construct` in FIELD order.
- **A check diagnostic is not an `sgl::diagnostic`.** It is a `located_diagnostic`: a module has several files, so it names one, and it carries a detail.
- **An emit error is no diagnostic.** It is an `emit::error`: a kind, the symbol it is about, and a detail; it has no span yet.
- **Addresses are positions.** Member i of an edge struct is location i, counted over the members without `@position`.
  What a struct is — vertex input, stage link, render targets — comes from where it stands in the signature, not from its attribute.
- **A name is renamed per target, an entry point never.** `target` is `target_` in WGSL only; an entry point named `filter` is an error in EVERY target.
- **Only an `@inline binding` is emitted**: `register(b0, space9)`, `[[vk::push_constant]]`, `@group(3) @binding(0)`.
  Its members must land on the same offsets in HLSL and in WGSL, so `{float; float3}` is `layout-mismatch` and `{float3; float}` is fine.
- **A new target is a `dialect`**: one file under `emit/impl/` and one case in `dialect_of`; the walk over the flat tree is shared (`impl/text_writer.cc`).
- **Every `sgl` fence under `docs/spec/` is a test** (`tests/spec/spec-examples-test.cc`): `sgl` must parse cleanly, `sgl error` must report the kind its lead names, `sgl sketch` is unchecked.
