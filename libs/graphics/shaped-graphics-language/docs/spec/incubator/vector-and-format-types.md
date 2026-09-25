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
normal = (..normal, 0) as vec4f16
```

The prefix `..` is the splat, which spreads the elements of `normal` into the list ([AST-28](../syntax/ast.md#lists)).

**The format types are not a special mechanism.**
`rgba8` and `vec4f16` will probably be prelude type defines, potentially `@builtin`.
So they are ordinary types that are also image formats, and one name serves in both roles:

```sgl sketch
binding instance:
    tex_color: texture2d[rgba8]     // a texture argument

@pixel struct pixel:
    color: rgba8                    // a framebuffer member
    normal: vec4f16
```

**A later reading, and the current one: a format is not a type yet.**
`rgba8` will be a type of four `u8` in `tg`, and it should not mean something else in SGL.
No target language returns a format either: a pixel shader returns floats, and the API converts on write.
That makes a shader polymorphic over its output format within limits, which is why a raster pipeline states the format it wants.
`rgba8`, `rgba16f` and `rgba32f` targets are all compatible with one shader, and SGL should not lose that, at least not without an escape hatch.

So a render target member has the type the shader computes, and the format is an optional attribute:

```sgl sketch
@pixel struct target:
    color: float4                     // any float target
    @format(rgba16f) normal: float4   // this one is pinned
```

A image format is an enum case with `sg`'s name, `image2d[.rgba8_unorm]` ([bindings.md](../bindings.md#image-formats)).
This is the spelling that is simplest to change later, and the sketches above predate it.

The same holds further down.
`int` and `float` are not keywords either: they are types with a `@builtin` annotation ([keywords.md](../keywords.md)).

The transformation hierarchy that `tg` has might be modelled later as well, instead of bare `mat4` products.

## What it touches

* The prelude: it declares both families and the format types, in SGL where that is possible.
* The type system: which operators each family has, and which conversions `as` offers between and within the families.
* `@builtin`: the annotation that ties a prelude declaration to something the compiler and the targets know.
* The transpiler: a format type needs a image format and a computation type in every target.
* [Host code generation](host-code-generation.md): a `tg` mirror maps to its `tg` type, and a format type to a format the host knows.
* [Stage interfaces](stage-interfaces.md): framebuffer members are format types.

## Already fixed by the syntax

* The splat is the prefix operator `..`, as a whole element of a paren group.
* `as` is a word operator, and its right-hand side is a type position ([types-as-values.md](types-as-values.md)).
* Type arguments are a fused square list, so `texture2d[rgba8]` needs nothing new.
* A symbol may end in digits, so `float3`, `vec4f16` and `rgba8` are plain identifiers.
* No type name is a keyword.

## Open

* Whether `hpos4` exists, and how much of `tg` is mirrored beyond `vec` and `pos`.
* Which conversions are implicit, if any: between `float3` and `vec3`, and from a computation type to a format type at a `return`.
* What `color as rgba8` does to the value: clamp, quantize, or only retype while the target quantizes on write.
* What arithmetic a format type has, or whether it must be converted before any use.
* The naming scheme of format types, should they exist beside the image formats, which [bindings.md](../bindings.md#image-formats) settles as enum cases with `sg`'s names.
* Whether the transformation hierarchy of `tg` is modelled, so that `mvp * v.pos` is typed by spaces.
