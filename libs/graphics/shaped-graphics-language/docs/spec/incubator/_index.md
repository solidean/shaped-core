# SGL Spec Incubator

Ideas recorded so they are not lost.
**Nothing here is normative**, and nothing here is promised.

An idea lands here when it came up while designing something else and is too good, or too consequential, to leave in a conversation.
It leaves when it is specified: the text moves into `syntax/`, `semantics/` or wherever it belongs, in the spec's firm register, and the incubator file is deleted.

Each file has the same shape, so an idea can be picked up cold:

* **The idea** — what it is, in the designer's framing.
* **What it touches** — which phases and which parts of the spec would change.
* **Already fixed by the syntax** — what the settled syntax has reserved or decided with this idea in mind.
* **Open** — what nobody has decided.

## Ideas

| file | the idea in one line |
|---|---|
| [structural-types.md](structural-types.md) | paren literals build structural types, named types are nominal, and a struct synthesizes its own constructor function |
| [function-model.md](function-model.md) | no recursion and no indirect calls, so every function inlines, nested functions need no captures, and `[]` means deducible |
| [shader-logging.md](shader-logging.md) | `print` and `assert` with full interpolation, drained from GPU buffers and formatted on the CPU, with exact stack traces |
| [ranges-and-iteration.md](ranges-and-iteration.md) | `range` as a builtin type, `in` as a membership test, and custom iterators for voxel and grid tracing |
| [user-operators.md](user-operators.md) | user-declared prefix, infix and postfix operators, placed by a small set of rules |
| [string-family.md](string-family.md) | raw `"""` strings and language-tagged strings such as `"json` |
| [beyond-shaders.md](beyond-shaders.md) | SGL as a test run for a general-purpose language, and what that asks of today's decisions |
