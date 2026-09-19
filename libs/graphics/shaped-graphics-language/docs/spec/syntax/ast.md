# AST

**DRAFT.**
This file is not normative yet, and it carries no rule ids.
It records how the [form tree](forms.md) is read into the program, as far as that is settled.
Back to the [phases](_index.md).

## From forms to the AST

The form tree holds few kinds of node: literals, identifiers, applications and calls, operator runs, keyword forms and composites.
The AST is an interpretation of that tree, and it gives each form a concrete meaning.
A form that has no meaning in its place is a normal error.
An AST node accepts a narrow set of attributes, and every other attribute is a normal error and is ignored.

Most declarations are keyword forms, often with a block.

```sgl
struct my_vec2:
    x: float
    y: float

enum color:
    red
    green
    blue

fun foo(a: int) -> int:
    return a + 2

fun baz() => 1 + 2

binding frame:
    dt: float

let x
let y : int
let z = 10
let (u, v) = (1, 2)
```

`let z = 10` is an assignment form whose left side is the keyword form `let z`, and the AST reads the pair as one declaration.

## Expressions

* literal
* identifier
* call; every operator resolves to a call of a function with a special name
* subscript
* member access, `v.x`
* `return <expr>?`, `break <expr>?`, `continue`
* `loop`; `for` and `while` are no expressions, and `loop` with `break <expr>` is one
* `case`; `if` is no expression

## Statements

* `if`, `for`, `while`
* `assert`, `print`
* `use`, `notation`

## What the AST decides

These readings need names or context, so the form tree leaves them open.

* An applied square group is type arguments when its head resolves to a type or a generic function, and a subscript otherwise.
* `[]` marks arguments that can be deduced; a compile-time function called with `()` also works on types, and gets no deduction.
* An assignment directly inside a paren group is a named argument.
* `->` stands only in function types and return types.
* A `for` loop is only `for <var> in <range>:`, and range is a builtin type.
* `in` outside a `for` is a membership test.
* Interpolation, single-quoted literals and backquoted literals are rejected until their semantics exist.

```sgl
let inside = x in 0..<1
for i in 0..<count:
    print "item $i"
```

## Semantic notes

These are short on purpose; the detail lives in the [incubator](../incubator/_index.md).

* Paren literals build structural types, and named types are nominal ([structural types](../incubator/structural-types.md)).
* `(1, 2)` has the members `.0` and `.1`, and `(1, b = 2)` has `.0` and `.b`.
* `{a = 1, b = 2}`, `{b = 2, a = 1}` and `(a = 1, b = 2)` are one type.
* `{a, b = 2}` is short for `{a = a, b = 2}`, which is the only difference between `{}` and `()`.
* A `struct` synthesizes a function of its own name, so `A(2)` is an ordinary call and there are no user constructors.
* A structural value converts implicitly where the members line up.
* There is no recursion and there are no indirect calls: every function is called statically and inlined ([function model](../incubator/function-model.md)).
* A nested function may use everything in scope, and it captures nothing.

## Open

* Partial assignment, `x .= {.1 = 8}`, is an idea only.
* `let` or `var` for a variable declaration ([keywords](../keywords.md)).
