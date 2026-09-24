# Generated Host Code

*Incubator: not normative.*

## The idea

A shader has edges that the host must describe a second time: what goes in as vertices, what comes out as render targets, and which resources each binding group holds.
SGL declares all three, and **the SGL tooling exports the host side from them**.

```sgl sketch
@vertex struct basic_vertex:
    pos: pos3
    normal: vec3
    @per_instance offset: vec3

@pixel struct gbuffer:
    color: float4
    normal: float4

binding frame:
    view_projection: mat4
    lights: buffer[light]
```

* **A `@vertex struct` becomes a C++ aggregate and its vertex input layout.**
  Members marked `@per_instance` or `@stream(name)` split it over several buffers, one C++ struct each, with a typed `buffers` struct to bind them.
* **A `@pixel struct` becomes a struct of named color targets**, which converts into a rendering and whose `states` fill a pipeline's color targets.
  Both carry the struct's name, so a pipeline built for one target set refuses a rendering of another.
* **A `binding` becomes a struct of typed views and plain fields**, and its plain members are the group's own constant buffer.
  It fixes no group index: an entry point numbers it by position in its list.

So the layout is written once, in the shader, and the host code that must agree with it is derived rather than maintained.
[stage-interfaces.md](stage-interfaces.md) is the shader-side half of the same structs.
slib's [cheat sheet](../../../../shaped-shader-library/cheat-sheet.md) shows what the build generates today.

**Names are mirrored into the host language.**
A binding `frame_data` becomes a struct in C++.
That is why `-` is not a symbol character, and why snake_case is the convention on both sides: no host language can spell a dash in a name.

## What it touches

* The SGL tooling: `sgl describe` reports the checked program, and the build generates C++ from that report.
* `sg`: the generated code targets its vertex input, render target and binding descriptions, and invents no API of its own.
* The type system: every type that may appear in a mirrored struct needs one defined host type and one defined layout.
* [Vector and format types](vector-and-format-types.md): `pos3` mirrors a `tg` type, and `rgba8` names a format the host knows.
* [Binding effects](binding-effects.md): the binding list of an entry point is what numbers its groups.
* A `pipeline` declaration, which states a whole pipeline in SGL: [pipelines.md](../pipelines.md).

## Already fixed by the syntax

* `-` is not a symbol character, so every SGL name is spellable in the host languages.
* Names are written in snake_case.
* Attributes attach by position, so `@vertex struct` and `@pixel struct` need no grammar of their own.
* Member order of a `struct` block is source order, which is what target order and vertex layout rest on.

## Open

* Whether a name that is a keyword in a host language is an error, a warning, or mangled.
* Which host languages besides C++ are targets.
* A shader used both with one vertex buffer and split over several, which the generated convenience does not cover.
