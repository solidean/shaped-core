# SGL Keywords

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
* `type` - declares a type alias or a template arg

Control flow and expressions:

* `if` - branching
* `else` - branching else
* `for` - looping
* `while` - looping
* `in` - full loop syntax is `for <var> in <a>..<b>:` where `..` is either `..<` or `..=` but never plain `..`
* `return` - returns from a function
* `continue` - continue next loop iteration
* `break` - breaks from loop iteration
* `case` - generalized if expression
* `as` - used for naming imports ("use our-materials as mat") and conversions "i as float"
* `and`/`or`/`not` - logical connectives

Special support:

* `assert` - assertions and test checks
* `print` - logging, printing, errors, warnings

(we could provide them as builtin functions with special parsing support BUT they are compiled out in production code so I want it to be visually clear that they are special)

TODO:

* `in` / `out` / `inout` / `ref` - not sure if we need these yet
* do we need `buffer` and `texture` etc as keywords? or are they simply types?
* do we need a `sampler` keyword?
* how do we distinguish 
* do we need an `inline` keyword or is it a `@inline` annotation (for push/inline constants)

Notes:

* `int` and `float` etc. are not keywords, they are types with a `@builtin` annotation

