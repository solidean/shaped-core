# Sum types

Variants as cases of a `struct`, nested to any depth, with a discriminator the compiler places or the program names, and narrowing through `is`.

## The idea

**One declaration form holds both fields and variants.**
A `struct` holds fields, and a `case` inside it adds a variant: a struct of its own that shares every field declared above it.
A case may hold cases, and a case that does is the set of the variants below it.
The word is deliberate: a struct's cases are what a `case` expression later takes apart.

```sgl sketch
struct material:
    roughness: float

    case dielectric:
        ior: float
        case glass:
            absorption: float3
        case thin_glass
    case metal:
        f0: float3
    case emissive:
        radiance: float3

let g = material.glass(roughness = 0.1, ior = 1.5, absorption = float3(0.2))
```

`material.dielectric` is a type, the set of `glass` and `thin_glass`, and `material.glass` is one variant.
A value of a struct with cases is always in exactly one of its leaf cases; a struct that needs a "none" state declares an empty case for it.
A struct holds itself neither directly nor through a case, since nothing on a GPU points: a tree stays indices into a buffer.

**An enum is a struct whose cases carry nothing**, each with a `@when` of equality on the enum's value.
Nothing in the language can use one where the other would not do, so whether the compiler represents an enum that way is its own business.
`bool` is a prelude enum already ([CHK-222](../semantics/checking.md#consts)), so a predicate over a `bool` field is a predicate over an enum.

**A field declared above a case sits at one offset in every variant below it.**
Reading it is the same load whatever the variant is, with no branch.

**A value is not its memory.**
A local of a sum type belongs to the compiler: its tag plus each variant's fields, split as any struct local is, and the target's liveness overlaps what never lives at once.
Memory — a buffer element, and the C++ struct the host sees — has a fixed layout, as large as the largest variant.
No target has a usable union, and WGSL cannot reinterpret memory, so the variant region is a run of `u32` words read and written by bitcast, beside typed common fields.

**By default the tag is the compiler's, and it goes to the first niche it finds:**

1. A padding word the layout leaves anyway, such as the one after a `float3` at natural alignment.
2. Values an enum field never takes.
3. Spare bits of an integer whose range is declared, once a range can be declared.
4. A word of its own, when nothing above is free.

A float is never a niche: WGSL may treat a NaN or infinity result as an indeterminate value, so its bits carry no tag.
Where the tag lands is deterministic and documented, since the host reads the same bytes.
`@padding(.none)` promises C++'s layout, which leaves fewer padding words to take.

**Or the program names the discriminator**, with a `@when` on each case that is a predicate over common fields alone:

```sgl sketch
struct material:
    kind: material_kind
    two_sided: bool
    roughness: float

    @when(kind == .glass and two_sided)
    case thin_glass:
        ior: float
    @when(kind == .glass and not two_sided)
    case glass:
        ior: float
        absorption: float3
    @when(kind == .metal)
    case metal:
        f0: float3
```

A variant's own fields never decide which variant a value is, since reading them already assumes the answer.

* **Disjointness and coverage are checked** where a predicate compares enum and integer fields against constants, joined by `and`, `or` and `not`.
  That is a finite-domain problem per field.
  An `int` field has no finite domain, so coverage over one needs a `_` arm, while disjointness is still checked.
* **A case is constructible only where its predicate is a conjunction of equalities**, from which the compiler writes the discriminating fields itself.
  Any other case is a view: `is` and `case` see it, and nothing builds it.
* **A field a predicate reads is read-only through the sum type**, since assigning it would change the variant under a reader.
  Only a whole value changes the variant.
* **`@unchecked` admits a predicate outside the checked fragment.**
  Its arms test in declaration order, the first match winning, and a `case` over it always needs a `_`.

**Or the tag is nowhere in the value**: it lives in another buffer, or it is the hit kind the hardware reports.
Such a struct is untagged — built and cast, never asked `is`.
It is also how a GPU program avoids paying for its largest variant: a tag and an index into one buffer per kind.

**`is` narrows, flow-sensitively, from the first version on.**
`if m is .glass:` makes `m` a `material.glass` inside the branch, so `m.absorption` reads, and an early return narrows the rest of the body.
A `case` arm narrows its scrutinee the same way.
Destructuring waits for [patterns.md](patterns.md); narrowing needs none of it.

**`as` downcasts without a check**: `m as material.glass` is the cast for a variant the program knows from elsewhere.
It accepts undefined behavior as `n as light_kind` does in [enum-futures.md](enum-futures.md).
`SC_SHADER_UNDEFINED` is its safety net: under `REPORT` the cast tests the tag and fails as a shader assert at its `.sgl` line, and under `NONE` it costs nothing.
An untagged struct has nothing to test, so its cast is unchecked under every profile.

**A set of variants is a type**: `material.glass or material.metal`, in the words a `case` arm already uses for `.spot or .sun`.
A set has the whole struct's layout, so narrowing to it is free and widening from it is implicit.
A `case` over a set is exhaustive with fewer arms.
A case that holds cases is already a named set, so `or` is for the sets that cut across the nesting.

**`T?` is a struct with two cases**, `none` and `some`, as large as `T` wherever `T` has a niche.
`triangle_hit`'s `is_hit` is that niche written by hand, and a mixed hit is the first real sum type ([raytracing-futures.md](raytracing-futures.md)).

**A `case` over a sum type costs what its branch costs, and nothing more.**
A uniform discriminator makes it a uniform branch ([uniformity.md](uniformity.md)), and a tag the compiler placed is a field like any other to that analysis.

**C++ sees common fields as members, then a union of each variant's payload**, nested as the cases nest.
`is_glass()` is the discriminator in C++, `as_glass()` a reference behind `CC_ASSERT`, and `material::glass({...})` a factory wherever the case is constructible.
A tag in a padding word is a named member in C++, since a C++ copy need not keep padding bytes.
Bytes read back from the GPU arrive by `bit_cast` or `memcpy`, never by reading a member that is not active.

## What it touches

* The AST: `case` as a declaration inside a struct block, nested, and `is` as a word operator.
* The check pass: the variant tree as a type kind, sets and their conversions, flow-sensitive narrowing, and a constructor per constructible case.
  Also the predicate fragment with its disjointness and coverage check, and the read-only discriminating fields.
* Layout: niche placement, the common fields' offsets and the word region, in the [layout rules](../semantics/emitting.md#layout) beside `@padding(.none)`.
* Emitting: a local split per variant, a buffer element as typed common fields plus `u32` words behind bitcasts.
* Legalization: a `case` over a tag with constant arms is a `switch`, and one over `@when` predicates an if-else chain.
  A narrowing arm binds no new name, so unlike a destructuring one it may still join a `switch`.
* The interpreter, which evaluates `is`, the downcast's check and `@when` as the targets do.
* Host code generation ([host-code-generation.md](host-code-generation.md)): the C++ struct, its accessors and factories.
* `enum`, which becomes the payload-free special case without changing a rule that checks one today.

## Already fixed by the syntax

* `case` is a keyword, and a struct block holds members rather than statements, so `case name:` inside one can only declare.
* An arm names several cases as `.spot or .sun` already, which is the spelling a set of variants takes in a type position.
* `as` is a `cast` node in the AST already.
* Attributes are an open, additive set, so `@when` and `@unchecked` need no grammar of their own.
* `bool` is an enum of the prelude, so a `bool` field is a finite domain like any enum field.

## Open

* How an untagged struct is spelled, since leaving out every `@when` already means a tag the compiler places.
* Whether `is` has a negated form, `m is not .glass`, or only `not (m is .glass)`.
* Whether an inner case's `@when` conjoins with its outer case's, or must restate it.
* Whether a field may be read on a set whose every variant declares it at the same offset with the same type, without a common ancestor declaring it.
* Whether sibling cases may declare a field of one name and different types, reachable only once narrowed.
* Whether a payload may be written in parens, `case some(T)`, beside the block form.
* Whether integers gain declared ranges, which the third niche needs.
* Which slice comes first: sv's materials want `@when` from the start, a mixed hit wants niches and `T?`.
