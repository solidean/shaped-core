# Enum futures

Where enums go once `case` works: ordering, casts to and from `int`, bit flags, and the arms a `case` is allowed to leave out.

## The idea

An enum is a closed set of named `int` values that converts to nothing, which is what makes it worth having over `int`.
That strictness is right for the first version and wrong forever, and four relaxations are already named, each of which buys something the strict form refuses.

**Ordering.**
`<`, `<=`, `>` and `>=` synthesized from the declaration order, so a threshold over a quality level reads as a comparison rather than a chain of equalities.
Nothing about it is hard; it waits for a use.

**Casts both ways.**
`value as int` and `n as light_kind`, accepted without a check.
This is a shading language, so the performance of a bounds test on every conversion is not worth its safety: an `int` that names no case converts anyway, and the program that did it is on its own.
That is a deliberate acceptance of undefined behaviour, and the documentation says so where the cast is introduced rather than in a footnote.
It is also the relaxation most likely to come early, because a value arriving from a buffer is an `int` and has to become an enum somewhere.

**`@bitflags`.**
An attribute on the declaration that says the cases are bits rather than alternatives, which turns on `|`, `&` and a membership test and turns off the ordering above.
`channel_mask` in the AST's own examples is written this way already, with `red = 1`, `green = 2`, `blue = 4`.
Without it such a declaration is legal and its arithmetic is not, which is where the first version leaves it.

**`@exhaustive(false)`.**
On a declaration whose set is open — one mirroring a host enum that grows, or a value arriving from outside.
A `case` over such a type would stop needing a `_` to be accepted, and an emitter would stop writing a `default` arm on the targets that do not demand one.
WGSL demands one unconditionally, so the attribute never reaches its text; the saving is HLSL's and MSL's alone, which is a thin reason to build it before something needs it.

**Nested declarations** inside an `enum` block are `unsupported-yet` in the first version and are meant to work.
They are the same question `struct` has; an enum's properties and methods are [CHK-233](../semantics/checking.md#members-and-constructors).

## What it touches

* The check pass: a type kind that carries the relaxations as flags, the synthesized comparisons, and the cast expression `as`, which nothing implements yet.
* The builtin registry: `|` and `&` over a flags enum are operator records like any other, unless they are synthesized instead.
* Emitting: nothing new — every relaxation above is an `int` operation on a target, and the named constants are already there.
* [user-operators.md](user-operators.md), if `@bitflags` turns out to be better expressed as user-declared operators on the type than as an attribute.

## Already fixed by the syntax

* **AST-115** lets a case carry a value, `red = 1`, which is what a flags enum needs and an ordinary one does not.
* `as` is a `cast` node in the AST already, so the cast relaxation needs no syntax.
* Attributes are an open, additive set, so `@bitflags` and `@exhaustive(false)` need no grammar of their own.

## Open

* Whether ordering comes from declaration order or from the values, which differ once a case carries a written one.
* Whether a flags enum's `case` means equality or containment, which are different questions and only one of them is what a reader expects.
* Whether `as` is one cast expression for the whole language or something narrower introduced here.
* Whether an open enum is an attribute on the declaration or a property of the `case` that reads it.
