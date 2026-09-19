# SGL Keywords

The keyword table is data handed to the form parser ([FORM-2](syntax/forms.md#the-parser)).
A keyword is a symbol token like any other; the table is what makes it a keyword.
A keyword that leads a run starts a [keyword form](syntax/forms.md#keyword-forms).
Back to the [specification](_index.md).

Definitions:

* `fun` - declares a function; mandatory on a method too, and only a property is keyword-free ([AST-82](syntax/ast.md#members))
* `let` - declares a variable (TODO: or `var`?)
* `mut` - marks a variable as mutable
* `struct` - defines a new structure type
* `enum` - defines a new enum type
* `binding` - defines a new binding group
* `sampler` - declares a static sampler
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

* `assert` - assertions and test checks: `assert condition` or `assert condition, message`, nothing more ([AST-57](syntax/ast.md#assert-and-print))
* `print` - logging, printing, errors, warnings: exactly one message, with interpolation for the rest ([AST-58](syntax/ast.md#assert-and-print))
* `notation` - for "notation \phi => φ"

(we could provide them as builtin functions with special parsing support BUT they are compiled out in production code so I want it to be visually clear that they are special)

## Reserved names

A reserved name is no keyword: the form parser reads it as an identifier, and its meaning is fixed, so no declaration may take it.
It never starts a keyword form, so `self.x = 0` is an assignment.

* `self` - the receiver of a method or a property; the AST reads it as `self_ref` ([AST-13](syntax/ast.md#atoms))
* `true` / `false` - the two values of `bool`; the AST reads them as names, and name lookup gives them their meaning

TODO:

* `in` / `out` / `inout` / `ref` - not sure if we need these yet
* do we need `buffer` and `texture` etc as keywords? or are they simply types?
* how do we distinguish 
* do we need an `inline` keyword or is it a `@inline` annotation (for push/inline constants)

Notes:

* `int` and `float` etc. are not keywords, they are types with a `@builtin` annotation

