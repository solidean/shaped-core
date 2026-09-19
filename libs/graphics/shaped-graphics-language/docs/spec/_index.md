# SGL specification

SGL, the Shaped Graphics Language, is a shading language.
This specification states what the language is.
Back to the [library readme](../../readme.md).

## The map

| file | holds |
|---|---|
| [syntax/_index.md](syntax/_index.md) | the syntax, phase by phase: line tree, tokens, group tokens, form tree, AST |
| [syntax/diagnostics.md](syntax/diagnostics.md) | normal and fatal errors, and every diagnostic kind of the syntax |
| [syntax/why/](syntax/why/line-tree.md) | the reasons behind the syntax rules, one file per normative file, starting at the line tree |
| [keywords.md](keywords.md) | the keyword table |
| [notation.md](notation.md) | symbol replacements, `notation \phi => φ` |
| [terminology.md](terminology.md) | the glossary: each term has one meaning |
| [bindings.md](bindings.md) | binding groups |
| [incubator/_index.md](incubator/_index.md) | ideas recorded so they are not lost; nothing in it is normative |
| [archive/syntax-draft.md](archive/syntax-draft.md) | the first draft of the syntax, kept unchanged; the files above replace it |

`semantics/`, `targets/` and `extensions/` will sit beside `syntax/`; they do not exist yet.

## Principles

* **Indentation is structure.** It is decided before any token is read, and no token can change it.
* **Errors stay local.** A line affects only itself, its children, and the first token of its next sibling.
* **Parsing is phased.** Bytes, line tree, tokens, group tokens, form tree, AST: each phase is a pure function of the one before.
* **Parsing is lossless.** Every tree addresses the source by byte span, and the source is reproduced byte for byte.
* **Parsing is total.** Any sequence of bytes gives a tree and a list of diagnostics, and nearly every error is a normal error with one reasonable reading.
* **The form tree is generic.** It is close to independent of the language, the keyword table is data, and the AST is an interpretation of it.
* **Parsing is file by file.** More than one file matters only from name resolution on.
* **Diagnostics have stable names.** Each kind has a kebab-case name and a default severity that a project may change.

## How to read a rule

A normative file states what is, in the present tense, one rule per line.

* A rule starts with its id in bold, such as **LINE-11**: a prefix for the file and a number.
* An id is stable: a rule keeps its id, and ids are never renumbered.
* A new rule takes the next free number of its file, so the ids of a file may be out of order.
* An id names one statement that a test can check, and tests and diagnostics cite it.
* A rule that ends in a [why](syntax/why/line-tree.md#line-11) link has its reason in the `why/` file of the same name, under a heading that is the id.
* A normative file holds no reasons, no history and no alternatives; those are in `why/`.
* "must" states what the author of a program has to do; what the language does is stated plainly.
* A term in bold is defined at that place, and [terminology.md](terminology.md) lists every term.
* A table with the columns source and reads as shows how a piece of source is read; it is not a dump format.
* A file may end with a section **Open** that lists what is not decided yet, one point per line.

## Examples are checked

The info string of a fenced example says what a test does with it.

| fence | the example |
|---|---|
| `sgl` | parses with no diagnostics |
| `sgl error` | reports at least one diagnostic, and the sentence before it names the kind |
| `sgl sketch` | is not checked: it depends on a later phase, on planned syntax or on an open point |

An example is checked through the form tree.
What only the AST reports does not make an `sgl` example fail yet, so an example the AST rejects is an `sgl sketch`, and the sentence before it says what is reported.
