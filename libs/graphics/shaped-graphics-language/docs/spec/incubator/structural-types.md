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

**A struct is built by calling a function, and a literal converts by calling it.**
That part is normative now: the synthesized constructor is [CHK-239](../semantics/checking.md#members-and-constructors).
A literal written where a struct is expected converts by [CHK-81](../semantics/checking.md#literals).
What stays here is the structural world itself: what a tuple or an object is once it is a value rather than a literal written in place.

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
* Conversion rules between structural values, which CHK-83 leaves out: only a literal written in place converts today.

## Already fixed by the syntax

* Named arguments are written `name = value`, never `name: value`.
  A colon inside a paren list is always a type ascription, which removes the ambiguity of `a: v` depending on whether `v` names a type.
* A leading-dot form may be digits (`.0`) and may be assigned inside a literal (`.0 = 7`).
* `t.0.1` is two index accesses, never the number `0.1`.
* `A(2)` and `A (2)` differ: the first is a call with a fused list, the second applies `A` to one tuple.

## Open

* Whether `(a = 1, 2)` — an unnamed element after a named one — is allowed, and what its index is.
* How `[1, 2, 3]` relates to `array[int, 3]`: the same type, or convertible.
* `.=` does not tokenize as one operator today; "a dot followed by an operator character opens an operator" would be the additive change.
* Whether member order of a `{}` literal is observable anywhere, for example in buffer layout.
