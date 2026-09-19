# Vector Type Families and Format Types

*Incubator: not normative.*

## The idea

The prelude ([modules-and-prelude.md](modules-and-prelude.md)) predefines two families of vector types.

* **`float3` and its siblings are component-wise types with otherwise weak semantics.**
* **`vec3`, `pos3` and maybe `hpos4` are mirrors of `tg`, with strong semantics.**
  In `tg`, the repo's typed-geometry library, a position, a vector and a homogeneous position are different types.

```sgl sketch
@vertex struct basic_vertex:
    pos: pos3        // a position
    normal: vec3     // a direction
    uv: vec2

let clip : hpos4 = mvp * v.pos
```

**Explicit conversion is preferred**, and `as` spells it:

```sgl sketch
normal = (mvp as mat3) * v.normal
color = color as rgba8
normal = (normal.., 0) as vec4f16
```

**The format types are not a special mechanism.**
`rgba8` and `vec4f16` will probably be prelude type defines, potentially `@builtin`.
So they are ordinary types that are also storage formats, and one name serves in both roles:

```sgl sketch
binding instance:
    tex_color: texture2d[rgba8]     // a texture argument

@pixel struct pixel:
    color: rgba8                    // a framebuffer member
    normal: vec4f16
```

The same holds further down.
`int` and `float` are not keywords either: they are types with a `@builtin` annotation ([keywords.md](../keywords.md)).

The transformation hierarchy that `tg` has might be modelled later as well, instead of bare `mat4` products.

## What it touches

* The prelude: it declares both families and the format types, in SGL where that is possible.
* The type system: which operators each family has, and which conversions `as` offers between and within the families.
* `@builtin`: the annotation that ties a prelude declaration to something the compiler and the targets know.
* The transpiler: a format type needs a storage format and a computation type in every target.
* [Host code generation](host-code-generation.md): a `tg` mirror maps to its `tg` type, and a format type to a format the host knows.
* [Stage interfaces](stage-interfaces.md): framebuffer members are format types.

## Already fixed by the syntax

* `as` is a word operator, and its right-hand side is a type position ([types-as-values.md](types-as-values.md)).
* Type arguments are a fused square list, so `texture2d[rgba8]` needs nothing new.
* A symbol may end in digits, so `float3`, `vec4f16` and `rgba8` are plain identifiers.
* No type name is a keyword.

## Open

* Whether `hpos4` exists, and how much of `tg` is mirrored beyond `vec` and `pos`.
* Which conversions are implicit, if any: between `float3` and `vec3`, and from a computation type to a format type at a `return`.
* What `color as rgba8` does to the value: clamp, quantize, or only retype while the target quantizes on write.
* What arithmetic a format type has, or whether it must be converted before any use.
* The naming scheme of the format types, and whether it follows the format names of `sg`.
* A postfix `..` as a splat operator, as in `(normal.., 0)`.
* Whether the transformation hierarchy of `tg` is modelled, so that `mvp * v.pos` is typed by spaces.
