# SGL Keywords

The keyword table is data handed to the form parser ([FORM-2](syntax/forms.md#the-parser)).
A keyword is a symbol token like any other; the table is what makes it a keyword.
A keyword that leads a run starts a [keyword form](syntax/forms.md#keyword-forms).
Back to the [specification](_index.md).

Definitions:

* `fun` - declares a function
* `let` - declares a variable (TODO: or `var`?)
* `mut` - marks a variable as mutable
* `struct` - defines a new structure type
* `enum` - defines a new enum type
* `binding` - defines a new binding group
* `const` - real constants
* `use` - import other modules
* `module` - declares a module
* `type` - declares a type alias or a template arg (or denotes the type of types)

Control flow and expressions:

* `if` - branching
* `else` - branching else
* `for` - looping
* `while` - looping
* `loop` - looping
* `return` - returns from a function
* `continue` - continue next loop iteration
* `break` - breaks from loop iteration
* `case` - generalized if expression

Word operators:

These never start a keyword form; they are operators of the [precedence ladder](syntax/operators.md#the-precedence-ladder).

* `in` - membership in a range, `x in 0..<1`; the full loop syntax is `for <var> in <a>..<b>:` where `..` is either `..<` or `..=` but never plain `..`
* `as` - names an import (`use our_materials as mat`) and converts a value (`i as float`)
* `and` / `or` / `not` - logical connectives, one precedence level

Special support:

* `assert` - assertions and test checks
* `print` - logging, printing, errors, warnings
* `notation` - for "notation \phi => φ"

(we could provide them as builtin functions with special parsing support BUT they are compiled out in production code so I want it to be visually clear that they are special)

TODO:

* `in` / `out` / `inout` / `ref` - not sure if we need these yet
* do we need `buffer` and `texture` etc as keywords? or are they simply types?
* do we need a `sampler` keyword?
* how do we distinguish 
* do we need an `inline` keyword or is it a `@inline` annotation (for push/inline constants)

Notes:

* `int` and `float` etc. are not keywords, they are types with a `@builtin` annotation

