# SGL syntax

The syntax of SGL is a chain of phases.
Each phase is a pure function of the output of the phase before it.
Back to the [specification](../_index.md).

```text
bytes -> line tree -> tokens -> group tokens -> form tree -> AST
```

## The phases, in order

| phase | input to output | file | rule ids |
|---|---|---|---|
| line tree | bytes to a tree of lines, by indentation alone | [line-tree.md](line-tree.md) | `LINE` |
| tokens | each line to tokens, in the mode its parent hands down | [tokens.md](tokens.md), [strings-and-comments.md](strings-and-comments.md) | `TOK`, `STR`, `CMT` |
| group tokens | tokens to short flat runs: parentheses matched, lines folded, attributes attached | [groups.md](groups.md) | `GRP` |
| form tree | each run to a form, by one generic precedence ladder | [forms.md](forms.md), [numbers.md](numbers.md), [operators.md](operators.md) | `FORM`, `NUM`, `OP` |
| AST | forms to declarations, statements and expressions, without looking a name up | [ast.md](ast.md) | `AST` |

[diagnostics.md](diagnostics.md) defines normal and fatal errors and lists every diagnostic kind, with the ids `DIAG`.
Each normative file has a twin of the same name in `why/` that holds the reasons, for example [why/line-tree.md](why/line-tree.md).

## Invariants every phase shares

* **SYN-1** Each phase is a pure function of the output of the phase before it.
* **SYN-2** Each phase is total: any sequence of bytes gives a tree and a list of [diagnostics](diagnostics.md).
* **SYN-3** Each phase is lossless: every node addresses the source by a byte span, and the source is reproduced byte for byte from the tree.
* **SYN-4** Whitespace is the gap between two tokens, and no node stores it.
* **SYN-5** Indentation is structure, and the structure is fixed before any token is read ([LINE-21](line-tree.md#line-kinds)).
* **SYN-6** No syntax error escapes its indentation: a line affects only itself, its children, and the first token of its next sibling ([LINE-23](line-tree.md#locality)).
* **SYN-7** Parsing is file by file, and more than one file matters only from name resolution on.
* **SYN-8** The form tree is generic, and the keyword table is data handed to the form parser ([FORM-2](forms.md#the-parser)).
* **SYN-9** The subtree of a line parses into one subtree of forms, so sibling statements parse independently.

## The three trees

| tree | nodes | built by |
|---|---|---|
| line tree | lines | the line tree phase |
| form tree | forms | the form phase, from group tokens |
| AST | declarations, statements, expressions | the AST phase |

The output of the grouping phase is not a tree of its own: it is the line tree with each run of tokens replaced by group tokens.

## One example through the phases

```sgl
fun area(size: vec2) -> float:
    // width times height
    return size.x * size.y
```

| phase | result |
|---|---|
| line tree | one top-level line with two children |
| tokens | line 1: `fun` `area` `(` `size` `:` `vec2` `)` `->` `float` `:`; line 2: one comment; line 3: `return` `size` `.` `x` `*` `size` `.` `y` |
| group tokens | line 1: `fun` `area` `(…)` `->` `float`, with the children as a block; the comment is attached, not in a run |
| form tree | keyword form `fun` with the argument `area(size : vec2) -> float` and a composite of one form: keyword form `return` with `size.x * size.y` |
| AST | a function declaration with one parameter, a return type and a block of one `return` expression |
