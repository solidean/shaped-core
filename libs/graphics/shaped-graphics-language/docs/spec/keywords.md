# SGL Keywords

The keyword table is data handed to the form parser ([FORM-2](syntax/forms.md#the-parser)).
A keyword is a symbol token like any other; the table is what makes it a keyword.
A keyword that leads a run starts a [keyword form](syntax/forms.md#keyword-forms).
Back to the [specification](_index.md).

Definitions:

* `fun` - declares a function; mandatory on a method too, and only a property is keyword-free ([AST-82](syntax/ast.md#members))
* `fun` without a name - an anonymous function in expression position, `fun (x) => x + 1` ([AST-101](syntax/ast.md#lambdas-case-and-loop))
* `let` - declares a variable (TODO: or `var`?)
* `mut` - marks a variable as mutable, and a binding member as read-write: `dst: mut buffer[float]` ([AST-128](syntax/ast.md#types))
* `out` - marks a binding member as write-only, which only an image is ([bindings](bindings.md#access))
* `struct` - defines a new structure type
* `enum` - defines a new enum type
* `binding` - defines a new binding group
* `sampler` - declares a static sampler, at file scope or in a binding; in a type position it denotes the sampler type ([bindings](bindings.md#samplers))
* `pipeline` - declares a pipeline: its stages and the configuration compiled into it ([pipelines](pipelines.md))
* `test` - declares a test: a body the check pass runs on its own, where a line of type `bool` is a check ([AST-138](syntax/ast.md#declarations))
* `const` - real constants
* `use` - import other modules
* `require` - declares the non-portable features a file, a binding or a body may use, as `sg::feature` names them ([Features](semantics/checking.md#features))
* `module` - declares a module
* `type` - declares a type alias or a template arg (or denotes the type of types)

Control flow and expressions:

* `if` - branching
* `else` - branching else
* `for` - looping
* `while` - looping
* `loop` - looping
* `return` - leaves the nearest enclosing `fun`, named or anonymous ([AST-112](syntax/ast.md#jumps))
* `yield` - **experimental**; gives a value block its value: `yield expression` ([AST-108](syntax/ast.md#value-blocks-and-yield))
* `continue` - continue next loop iteration
* `break` - breaks from loop iteration
* `case` - generalized if expression

Word operators:

These never start a keyword form; they are operators of the [precedence ladder](syntax/operators.md#the-precedence-ladder).

* `in` - membership in a range, `x in 0..<1`; the full loop syntax is `for <var> in <a>..<b>:` where `..` is either `..<` or `..=` but never plain `..`
* `as` - names an import (`use our_materials as mat`) and converts a value (`i as float`)
* `and` / `or` / `not` - logical connectives, one precedence level

Special support:

* `assert` - assertions and test checks: `assert condition` or `assert condition, message`, nothing more ([AST-57](syntax/ast.md#assert-and-print))
* `print` - logging, printing, errors, warnings: exactly one message, with interpolation for the rest ([AST-58](syntax/ast.md#assert-and-print))
* `notation` - for "notation \phi => φ"

(we could provide them as builtin functions with special parsing support BUT they are compiled out in production code so I want it to be visually clear that they are special)

## Reserved names

A reserved name is no keyword: the form parser reads it as an identifier, and its meaning is fixed, so no declaration may take it.
It never starts a keyword form, so `self.x = 0` is an assignment.

* `self` - the receiver of a method or a property; the AST reads it as `self_ref` ([AST-13](syntax/ast.md#atoms))
* `void` - the unit type in a type position, and its one value anywhere else; the AST reads it as `void_ref` ([AST-137](syntax/ast.md#atoms))

## Open

Nothing is open today.

Settled:

* `true` and `false` are no keywords: they are `@shadowable(false)` consts of the prelude over the cases of `bool` ([CHK-222](semantics/checking.md#consts)).
* `out` is a keyword and `in` / `inout` / `ref` are not: the only access a resource needs beyond read and `mut` is write-only.
* `buffer`, `bytes`, `constants`, the texture types and the image types are types rather than keywords, since they take type arguments like any other type.
* `sampler` stays a keyword even where it names a type, since the word is one concept: a sampler, static or bound.
* `@inline` stays an annotation: it says where a binding lives rather than what it is.

Notes:

* `int` and `float` etc. are not keywords, they are types with a `@builtin` annotation

