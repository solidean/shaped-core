# shaped-graphics-language cheat sheet

SGL's toolchain as a library: today the syntactic half, from bytes to the form tree.
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
file.lines  file.tokens  file.groups  file.forms  file.diagnostics   // flat arrays; every node is an index, -1 for none
file.first_line  file.root_block  file.root_form                      // where each tree starts
file.text_of(span)                         // -> cc::string_view into source
file.tokens_of(line)                       // -> cc::span<token const>
file.is_fused_left(token_index)            // -> bool: touches the token before it
file.form_attributes                       // attribute group indices; each form owns a contiguous range
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
sgl::line    // text, terminator_length, kind, indent_bytes/indent_columns, parent/first_child/next_sibling, first_token/token_count
sgl::token   // kind + where; NO keyword/number/identifier kinds: `10a7`, `@range`, `let` are all token_kind::symbol
sgl::group   // kind (token, round, square, curly, quoted, block, statement, attribute), is_fused_left, starts_line,
             // token/close_token, first_child/next_sibling, first_attribute
sgl::form    // kind, where, token, first_child/next_sibling, first_attribute/attribute_count
sgl::source_span   // { u32 offset, length }; end(), empty()
```

`form_kind`: `missing` `error` `number` `quoted` `hash_literal` `identifier` `wildcard` `keyword` `op`
`round_list` `square_list` `curly_list` `leading_dot` `member` `call` `application`
`prefix_operator` `postfix_operator` `operator_run` `keyword_form` `block` `sequence`.

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
- **`operator_run` is flat** for one precedence level (operands and `op` leaves alternate); assignment and `=>` hold two operands and nest right.
- **A form's attributes are not on its groups.** Read them through `first_attribute` / `attribute_count` into `file.form_attributes`.
- **Every `sgl` fence under `docs/spec/` is a test** (`tests/spec/spec-examples-test.cc`): `sgl` must parse cleanly, `sgl error` must report the kind its lead names, `sgl sketch` is unchecked.
