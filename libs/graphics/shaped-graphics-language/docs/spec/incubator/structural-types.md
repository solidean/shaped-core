# Structural Types and Synthesized Constructors

*Incubator: not normative.*

## The idea

The three paren literals create **unnamed types with structural typing**.
Named types, declared with `struct`, are **nominally** typed.

A literal's members are synthesized from its elements:

| literal | members |
|---|---|
| `(1, 2)` | `.0` `.1` — an `(int, int)` tuple |
| `(1, b = 2)` | `.0` `.b` |
| `{a = 1, b = 2}` | `.a` `.b` |
| `{b = 2, a = 1}` | the same type as the line above |
| `(a = 1, b = 2)` | the same type again |
| `{a, b = 2}` | shorthand for `{a = a, b = 2}`, so `a` must be in scope |
| `[1, 2, 3]` | indexed: `[0]` `[1]` `[2]` |

The shorthand is the **only** difference between `{}` and `()`.
Named elements are allowed in every position of all three literals.

**A struct has no user-written constructor.**

```sgl sketch
struct A:
    a: float
    b: float = 2
```

declares the type and synthesizes a function of the same name:

```sgl sketch
fun A(a: float, b: float = 2) -> A
```

**A field's default may use the fields before it.**
The defaults become the default arguments of the synthesized function, and default arguments are evaluated left to right.

```sgl sketch
struct falloff:
    radius: float
    inner: float = radius * 0.5
    sharpness: float = 1 / (radius - inner)
```

```sgl sketch
fun falloff(radius: float, inner: float = radius * 0.5, sharpness: float = 1 / (radius - inner)) -> falloff
```

Field order is the only rule this needs, and the AST checks none of it ([AST-116](../syntax/ast.md#members)).

Writing `A(2)` names that function rather than the type.
So construction is an ordinary function call with ordinary rules for defaults and named arguments.
Objects are always complete: the only partially initialized state exists inside the synthesized function, which is not user code.
There are no rules about partially constructed objects because no user can observe one.

**The structural world converts implicitly** wherever the members line up:

```sgl sketch
let mut x = (1, 2)
x = {
    .0 = 7
    .1 = 8
}
```

**Partial assignment** is a tentative extension of the same idea:

```sgl sketch
x .= {.1 = 8}
```

It is pointless for one member and pays off for several.

## What it touches

* The AST phase: an assignment directly inside a paren literal is a named element, not an assignment.
* The type system: structural equality up to member order for named members, positional for unnamed ones.
* Conversion rules between structural values, and from structural values to nominal types at a call.
* Overload and default-argument resolution, since struct construction is a call.
* Name resolution: a field default sees the fields declared before it, the way a default argument sees the parameters before it.

## Already fixed by the syntax

* Named arguments are written `name = value`, never `name: value`.
  A colon inside a paren list is always a type ascription, which removes the ambiguity of `a: v` depending on whether `v` names a type.
* A leading-dot form may be digits (`.0`) and may be assigned inside a literal (`.0 = 7`).
* `t.0.1` is two index accesses, never the number `0.1`.
* `A(2)` and `A (2)` differ: the first is a call with a fused list, the second applies `A` to one tuple.

## Open

* Whether `(a = 1, 2)` — an unnamed element after a named one — is allowed, and what its index is.
* Whether a structural value converts to a nominal struct implicitly, or only at a call to the synthesized function.
* How `[1, 2, 3]` relates to `array[int, 3]`: the same type, or convertible.
* `.=` does not tokenize as one operator today; "a dot followed by an operator character opens an operator" would be the additive change.
* Whether member order of a `{}` literal is observable anywhere, for example in buffer layout.
