# SGL Keywords

Definitions:

* `fun` - declares a function
* `let` - declares a variable (TODO: or `var`?)
* `mut` - marks a variable as mutable
* `struct` - defines a new structure type
* `enum` - defines a new enum type
* `binding` - defines a new binding group

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

TODO:

* `in` / `out` / `inout` / `ref` - not sure if we need these yet
* do we need `buffer` and `texture` etc as keywords? or are they simply types?
* do we need a `sampler` keyword?
* how do we distinguish 
* do we need an `inline` keyword or is it a `@inline` annotation (for push/inline constants)

Notes:

* `int` and `float` etc. are not keywords, they are types with a `@builtin` annotation

