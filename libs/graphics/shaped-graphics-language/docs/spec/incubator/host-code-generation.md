# Generated Host Code

*Incubator: not normative.*

## The idea

A shader has two edges that the host must describe a second time today: what goes in as vertices, and what comes out as render targets.
SGL declares both as annotated structs, and **the SGL tooling exports the host side from them**.

```sgl sketch
@vertex struct basic_vertex:
    pos: pos3
    normal: vec3
    uv: vec2

@pixel struct pixel:
    color: rgba8
    normal: vec4f16
```

* **A struct annotated with `@vertex` becomes a valid vertex input.**
  The tooling can export C++ code for the struct and for its vertex setup.
* **Packed and separate vertex inputs are both supported.**
  The annotated struct is the one that carries the vertex inputs `sg` needs.
* **A `@pixel struct` defines the framebuffer format**, with member order as target order.
  The tooling can auto-generate named C++ definitions to set up the targets.

So the layout is written once, in the shader, and the host code that must agree with it is derived rather than maintained.
[stage-interfaces.md](stage-interfaces.md) is the shader-side half of the same structs.

**Names are mirrored into the host language.**
A binding `frame_data` becomes a struct in C++.
That is why `-` is not a symbol character, and why snake_case is the convention on both sides: no host language can spell a dash in a name.

## What it touches

* The SGL tooling: an exporter that runs on the checked program, per host language, C++ first.
* `sg`: the generated code targets its vertex input, render target and binding descriptions, and invents no API of its own.
* The type system: every type that may appear in a mirrored struct needs one defined host type and one defined layout.
* [Vector and format types](vector-and-format-types.md): `pos3` mirrors a `tg` type, and `rgba8` names a format the host knows.
* [Binding effects](binding-effects.md): the binding groups of an entry point are the third thing worth mirroring.
* The build: shader packages already produce typed C++ symbols, and generated structs would arrive the same way.

## Already fixed by the syntax

* `-` is not a symbol character, so every SGL name is spellable in the host languages.
* Names are written in snake_case.
* Attributes attach by position, so `@vertex struct` and `@pixel struct` need no grammar of their own.
* Member order of a `struct` block is source order, which is what target order and vertex layout rest on.

## Open

* How packed and separate vertex inputs are told apart in the source: one struct per vertex buffer, or an annotation per member.
* Whether the generated C++ struct reuses `tg` types where the SGL type mirrors one, or stays plain.
* Whether binding groups and the pipeline layout are exported too, and in what shape.
* Whether a name that is a keyword in a host language is an error, a warning, or mangled.
* Which host languages besides C++ are targets.
* Where generated code lives in a build, and whether it is checked in.
* Per-instance vertex inputs.
