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
| [types-as-values.md](types-as-values.md) | everything is a value, and an expression in a type position must reduce to a normal form that reads as a type |
| [scopes.md](scopes.md) | ordered scopes such as functions, unordered ones such as structs and the root, and nested functions that capture nothing |
| [binding-effects.md](binding-effects.md) | each `binding` is a binding group, functions list the bindings they use, and a local `binding` rebinds for a library |
| [members-and-properties.md](members-and-properties.md) | structs and enums carry methods and read-only properties, `self` names the receiver, and no `self` means static |
| [stage-interfaces.md](stage-interfaces.md) | entry points by stage attribute, stage-to-stage types that must match, and attributes as an open, additive set |
| [host-code-generation.md](host-code-generation.md) | `@vertex` and `@pixel` structs and `binding` groups, from which the tooling exports C++ vertex setup, render targets and group structs |
| [modules-and-prelude.md](modules-and-prelude.md) | an optional `module` line, export by default, and a standard prelude that is added automatically |
| [vector-and-format-types.md](vector-and-format-types.md) | weak `float3`-style types beside strong `tg` mirrors, and format types as ordinary prelude types |
| [shader-logging.md](shader-logging.md) | `print` and `assert` with full interpolation, drained from GPU buffers and formatted on the CPU, with exact stack traces |
| [ranges-and-iteration.md](ranges-and-iteration.md) | `range` as a builtin type, `in` as a membership test, and custom iterators for voxel and grid tracing |
| [user-operators.md](user-operators.md) | user-declared prefix, infix and postfix operators, placed by a small set of rules |
| [string-family.md](string-family.md) | raw `"""` strings and language-tagged strings such as `"json` |
| [compilation-model.md](compilation-model.md) | shader text as the target, modules compiled in parallel against interfaces, and one demand-driven pass for names, types and evaluation |
| [feature-levels.md](feature-levels.md) | a function declares the non-portable features it needs, and checking refuses every undeclared use |
| [literal-types.md](literal-types.md) | a literal keeps a literal type throughout an expression, and coerces only where another type is asked of it |
| [inferred-comptime.md](inferred-comptime.md) | a function value is a compile-time entity, and a parameter that must be constant is inferred from the body, as Zig's `comptime` without the keyword |
| [patterns.md](patterns.md) | a real pattern language for `case`, where an arm destructures its scrutinee and binds the pieces it names |
| [enum-futures.md](enum-futures.md) | ordering, casts to and from `int`, `@bitflags` for a mask enum, and `@exhaustive(false)` for an open one |
| [texture-methods.md](texture-methods.md) | textures sampled through methods over UFCS, with a default sampler declared on the binding |
| [uniformity.md](uniformity.md) | SGL's own uniformity analysis after inlining, refusing a derivative sample in divergent control flow with a fix to offer |
| [beyond-shaders.md](beyond-shaders.md) | SGL as a test run for a general-purpose language, and what that asks of today's decisions |
| [testing.md](testing.md) | tests on the GPU against the interpreter, resource values in tests, captured constants and message matching |
