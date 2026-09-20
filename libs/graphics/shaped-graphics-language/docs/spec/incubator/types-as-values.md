# Types as Values

*Incubator: not normative.*

## The idea

This is almost a kind of bet.
The hope is that SGL never needs to distinguish a "type" kind from a "value" kind.
**Everything is a value, and there are type positions.**

A type position is the right-hand side of `->`, of `:` or of `as`, and the right-hand side of a `type` alias.
An expression that stands there is an ordinary expression, written and parsed like any other.

The semantic phase does partial evaluation.
After inlining and instantiation, every expression in a type position must reduce to a **normal form that can be interpreted as a type**.
In most cases that normal form is simply an identifier that resolves to a type.
It can also be a tuple or an object literal.

`(int, int)` is a value of type `tuple[type, type]`, and it can be interpreted as `tuple[int, int]`.
That is how this declaration works:

```sgl sketch
let a : (int, int) = (1, 2)
```

`(int, int)` is a value in a type position.
It is already in normal form, and it can be interpreted as a type.

**Runtime values must not contain the type `type` anywhere after all the inlining.**
That one rule is what separates the compile-time world from the surviving code.
Functions that return types are therefore no problem:

```sgl sketch
fun foo(a: bool) => case a:
    true => int
    false => vec2

let v : foo false = ...
```

This is fine in a shader, because `foo false` reduces to `int` before anything survives.
This is not, in surviving code:

```sgl sketch
let v : type = int
```

The bet rests on the [function model](function-model.md): every function inlines, so the partial evaluator always sees the whole call.

## What it touches

* The AST phase: it marks which expressions stand in a type position, and gives them no grammar of their own.
* The semantic phase: a partial evaluator that runs after inlining and instantiation, and a definition of the normal forms.
* The type system: `type` is the type of types, and `tuple[type, type]` is an ordinary instantiation.
* Diagnostics: an expression in a type position that does not reduce, and a `type` that survives into runtime code, are the two new errors.
* [Structural types](structural-types.md): a paren literal of types is what spells an unnamed type.

## Already fixed by the syntax

* Type arguments are a fused square list, the same form as a subscript: `buffer[float]` and `weights[3]` parse alike.
* The AST has one expression family, and it records which expressions stand in a type position ([AST-9 to AST-11](../syntax/ast.md#expressions)).
* A curly list of `name: type` elements reads as `struct_type` ([AST-30](../syntax/ast.md#types)).
* `[]` signals arguments a caller may omit and have deduced.
  A compile-time function that takes a type through `()` gets no deduction.
* `->` stands only in function types and return types, so its right-hand side is always a type position.
* `type` is a keyword, and one of its meanings is the type of types.

## Open

* The exact set of normal forms: identifiers, tuples and object literals certainly, and what else.
* How the `struct_type` node `{pos: hpos4, uv: vec2}` fits the normal forms: its members are fields, while `{int, int}` is a value of types.
* Whether an argument of a `[]` list is a type position, or becomes one only once the head resolves.
* How far the partial evaluator goes: which control flow and which builtin functions it must be able to run.
* What the diagnostic looks like when a type position does not reduce, given that the cause may sit several inlined calls away.
* Whether the bet holds: a construct that forces a separate type kind would end it.
