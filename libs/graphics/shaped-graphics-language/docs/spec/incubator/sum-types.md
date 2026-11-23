# Sum types: what is left

Structs with cases are specified, [checking.md](../semantics/checking.md#structs-with-cases) CHK-540 to CHK-552, and their carrier is EMIT-275.
What follows is what is left out, each part waiting for a use.

## The idea

**The compiler's tag takes more niches than a padding hole.**
Today it takes the first four-byte hole the common fields leave, and a word of its own where none does (CHK-544).
Two more niches stand next in line:

1. Values an enum field of a payload never takes, as Rust holds `Option<bool>` in one byte.
2. Spare bits of an integer field with a range, `index: uint in 0..<(1 << 24)` leaving 8 bits free.

A common field's spare values are no niche, since it holds its own value in every case; its spare bits are, beside that value.
A tag in an integer's spare bits costs every read of that field a mask, which is why it waits for a use that wants the word back.
A float is never a niche: WGSL may treat a NaN or infinity result as an indeterminate value, so its bits carry no tag.

**`T?` is a struct with two cases**, `none` and `some`, as large as `T` wherever `T` has a niche.
`triangle_hit`'s `is_hit` is that niche written by hand, and a mixed hit is the first real sum type ([raytracing-futures.md](raytracing-futures.md)).
`?` is a reserved operator already, so the spelling costs no grammar.

**A variant field of a 64-bit value or a matrix** is two words per lane, and a matrix's columns.
`reinterpret_as_uint` of a `double` gives a `uint2` already, so the double is a small step and the 64-bit integers want a split of their own.

**`.glass(…)` where a struct with cases is expected**, as a leading dot names an enum's case where one is expected.

## What it touches

* The check pass: niche placement in `compile_struct_cases`, and a mask in each read of a field the tag shares a word with.
* The AST: `T?` as a type.
* slib's generator: an accessor of the field a niche shares, which hides the tag's bits from the host.

## Already fixed by the syntax

* `?` is reserved, `reserved-operator` (OP-12), so `T?` takes no new token.
* A leading dot is a node of its own already, so `.glass(…)` is a call of one.

## Open

* Whether `T?` is a prelude struct the program could write, or the compiler's own.
* Whether a niche in a payload's field may change what a host writes there, which the host types would then have to say.
