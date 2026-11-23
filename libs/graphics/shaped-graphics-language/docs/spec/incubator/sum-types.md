# Sum types: what is left

Structs with cases are specified, [checking.md](../semantics/checking.md#structs-with-cases) CHK-540 to CHK-551, and their carrier is EMIT-275.
What follows is what the first version left out, each part waiting for a use.

## The idea

**The compiler's tag takes more niches than a padding hole.**
Today it takes the first four-byte hole the common fields leave, and a word of its own where none does (CHK-544).
Two more niches stand next in line:

1. Values an enum field of a payload never takes, as Rust holds `Option<bool>` in one byte.
2. Spare bits of an integer field with a range, `index: uint in 0..<(1 << 24)` leaving 8 bits free.

A common field is no niche for the tag, since it holds its own value in every case; the niche is a field of the payload the other cases do not hold.
A tag in an integer's spare bits costs every read of that field a mask, which is why it waits for a use that wants the word back.
A float is never a niche: WGSL may treat a NaN or infinity result as an indeterminate value, so its bits carry no tag.

**`T?` is a struct with two cases**, `none` and `some`, as large as `T` wherever `T` has a niche.
`triangle_hit`'s `is_hit` is that niche written by hand, and a mixed hit is the first real sum type ([raytracing-futures.md](raytracing-futures.md)).
`?` is a reserved operator already, so the spelling costs no grammar.

**A variant field of a 64-bit value or a matrix** is two words per lane, and a matrix's columns.
`reinterpret_as_uint` of a `double` gives a `uint2` already, so the double is a small step and the 64-bit integers want a split of their own.

**A part of a variant field as a place**, `m.f0.x = 1.0`, reads the field's words, changes the part and writes them back.
Today a field is assigned whole, and a part of one is `unsupported-yet` (CHK-543).

**`.glass(…)` where a struct with cases is expected**, as a leading dot names an enum's case where one is expected.

**A method or a property in a case's block**, valid for the values of that case, `self` narrowed to it.
Today such a member is `unsupported-yet`.

**Narrowing a member path**, `p.material is .glass`, where `p` is a name that is no `mut`.
Today only a name narrows (CHK-542).

**The host's reach of a struct-typed variant field**: its words are its leaves packed, which its host struct's layout is not, so it has no accessor yet.

## What it touches

* The check pass: niche placement in `compile_struct_cases`, the decode of a niched tag, and narrowing keyed by a path.
* The AST: `T?` as a type, and members of a case's block that are no field.
* The flattener: the read-modify-write of a part of a variant field.
* slib's generator: accessors over a struct-typed variant field.

## Already fixed by the syntax

* `?` is reserved, `reserved-operator` (OP-12), so `T?` takes no new token.
* A leading dot is a node of its own already, so `.glass(…)` is a call of one.

## Open

* Whether `T?` is a prelude struct the program could write, or the compiler's own.
* Whether a niche in a payload's field may change what a host writes there, which the host types would then have to say.
