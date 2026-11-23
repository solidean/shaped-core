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

**A payload may be written in parens**, positional or named, as a call's arguments are:

```sgl sketch
struct shape:
    case sphere(center: float3, radius: float)
    case plane(float3, float)
```

A positional payload's fields are `.0`, `.1`, as a tuple's are ([structural-types.md](structural-types.md)).
A paren payload may also have a block, and the two add up: the paren fields first, then the block's fields and cases.
Positional construction covers the paren fields alone, and the block's are named or take their defaults.

```sgl sketch
case sphere(center: float3, radius: float):
    emissive: bool = false
    case solid
    case hollow(thickness: float)
```

**Sibling cases are separate scopes**, so `glass.ior: float` and `metal.ior: float3` may coexist.
Each is reached once the value is narrowed to its case, and `m.ior` on the whole `material` is an error.
A name two siblings share is never a shared field by accident, whatever its types: sharing is declared, below.
A value of a struct with cases is always in exactly one of its leaf cases; a struct that needs a "none" state declares an empty case for it.
A struct holds itself neither directly nor through a case, since nothing on a GPU points: a tree stays indices into a buffer.

**An enum is a struct whose cases carry nothing**, each with a `@when` of equality on the enum's value.
Nothing in the language can use one where the other would not do, so whether the compiler represents an enum that way is its own business.
`bool` is a prelude enum already ([CHK-222](../semantics/checking.md#consts)), so a predicate over a `bool` field is a predicate over an enum.

**A field declared above a case sits at one offset in every variant below it.**
Reading it is the same load whatever the variant is, with no branch.

**A field may be shared by cases with no common ancestor that declares it**, by naming them:

```sgl sketch
struct material:
    roughness: float
    @cases(.glass, .metal)
    tint: float3
    case glass: ...
    case metal: ...
    case emissive: ...
```

`tint` has one offset in `glass` and in `metal`, and reads on any set within them, `material.glass or material.metal` among them.
`emissive` has no `tint`, and its own fields may occupy the words `tint` takes elsewhere.
`@cases` names sets as well as leaves, so `@cases(.dielectric, .metal)` gives `tint` to every case below `dielectric`.
Declaring it once at the owner keeps the layout decision in one place; a field declared in each case and unified afterwards could fail to fit, and say so far from the cause.

**A value is not its memory.**
A local of a sum type belongs to the compiler: its tag plus each variant's fields, split as any struct local is, and the target's liveness overlaps what never lives at once.
Memory — a buffer element, and the C++ struct the host sees — has a fixed layout, as large as the largest variant.
No target has a usable union, and WGSL cannot reinterpret memory, so the variant region is a run of `u32` words read and written by bitcast, beside typed common fields.

**By default the tag is the compiler's, and it goes to the first niche it finds:**

1. A padding word the layout leaves anyway, such as the one after a `float3` at natural alignment.
2. Values an enum field never takes.
3. Spare values and bits of an integer whose range is declared, `index: u32 in 0..<(1 << 24)` leaving 8 bits free.
4. A word of its own, when nothing above is free.

A float is never a niche: WGSL may treat a NaN or infinity result as an indeterminate value, so its bits carry no tag.
Where the tag lands is deterministic and documented, since the host reads the same bytes.
`@padding(.none)` promises C++'s layout, which leaves fewer padding words to take.

**A field's type may name a range**: `x: int in 0..<10`.
It parses already: `:` and `in` share level 7 and associate left ([OP-2](../syntax/operators.md)), so the line is `(x: int) in (0..<10)`.
What changes is the member table ([AST-78](../syntax/ast.md#members)), which gains a row: a membership whose left side is a field reads as a ranged field.
The same row serves a parameter and a `let`; in an expression the line keeps meaning an ascription, then a membership test.
A ranged field is an `int` with a fact attached, not a type of its own, so no type position spells the range.
Overloading and generics see `int`, and arithmetic on it gives a plain `int`: the range constrains only what may be stored.
A store into it needs a value the check pass can prove fits; otherwise `as` converts it, checked under `SC_SHADER_UNDEFINED`'s `REPORT` as a downcast is.

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

**A nested `@when` adds to its outer case's**, so the predicates nest the way checks and casts do.
`m is .glass` means `m is .dielectric`, then glass's predicate on `m` narrowed to `material.dielectric`.
An inner predicate may read the outer case's fields as well as the root's, since by then the value is known to have them.
A case's predicate reads the fields of its ancestors and never its own or its descendants', since reading those already assumes the answer.
Both are well-defined because every check along the path is one a program could write as a narrowing `if`.

* **Disjointness and coverage are checked** where a predicate compares enum and integer fields against constants, joined by `and`, `or` and `not`.
  That is a finite-domain problem per field.
  An `int` field has no finite domain, so coverage over one needs a `_` arm, while disjointness is still checked.
* **A case is constructible only where every predicate on its path is a conjunction of equalities**, from which the compiler writes the discriminating fields itself.
  Any other case is a view: `is` and `case` see it, and nothing builds it.
* **A field a predicate reads is read-only through the sum type**, since assigning it would change the variant under a reader.
  That holds for an outer case's field read by an inner predicate too, wherever the value is narrowed to that outer case.
  Only a whole value changes the variant.
* **`@unchecked` admits a predicate outside the checked fragment.**
  Its arms test in declaration order, the first match winning, and a `case` over it always needs a `_`.

**Or the tag is nowhere in the value**: it lives in another buffer, or it is the hit kind the hardware reports.
Such a struct is untagged — built and cast, never asked `is` — which `@tag(.external)` on the struct says.
It reads as `@padding(.none)` does, and the same attribute is where a forced placement would go, `@tag(.word)`.
It is also how a GPU program avoids paying for its largest variant: a tag and an index into one buffer per kind.

**`is` narrows, flow-sensitively, from the first version on.**
`if m is .glass:` makes `m` a `material.glass` inside the branch, so `m.absorption` reads.
`is not` is its negation, and narrows to the complement: after `if m is not .glass: return`, `m` is a `material.glass` for the rest of the body.
The complement of a set is a set, so a value narrowed away from one case is still a type the program can name.
`is not` is one operator of two words, since `not .glass` alone means nothing.
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

**Materials come first**, and a material's discriminator is the program's, so the first slice is the `@when` half:

* Cases inside a struct, nested, in the block form and the paren form.
* `@when` with nesting, its checked fragment, the disjointness and coverage check, and read-only discriminating fields.
* Constructors for the constructible cases.
* `is` and `is not` with flow-sensitive narrowing, and a `case` whose arms narrow.
* The `as` downcast, checked under `REPORT`.
* The memory layout — typed common fields and a word region — in buffers and in the C++ struct with its accessors.

The compiler's own tag comes next, first in a word of its own and then niched, since a material needs neither.
Sets as types, `@cases`, `@tag(.external)`, `@unchecked`, ranged integers and `T?` follow as their uses arrive, a mixed hit the first of them.

## What it touches

* The AST: `case` as a declaration inside a struct block, nested, `is` and `is not` as word operators, and the member row of a ranged field.
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
* Attributes are an open, additive set, so `@when`, `@unchecked`, `@cases` and `@tag` need no grammar of their own.
* `x: int in 0..<10` already parses as `(x: int) in (0..<10)` (OP-2), which a member row can read as a ranged field.
* `bool` is an enum of the prelude, so a `bool` field is a finite domain like any enum field.

## Open

Nothing is open today.
