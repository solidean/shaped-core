# Texture Methods and Default Samplers

*Incubator: not normative.*

## The idea

**A texture is sampled through methods, and nearly every texture has one sampler it is always sampled with.**
So the binding says which, and the call leaves it out:

```sgl sketch
sampler bilinear:
    filter = .linear

binding frame:
    @sampler(bilinear)
    sky: texture_cube[float3]

let color = frame.sky.sample(dir)
let base = frame.sky.sample(dir, level = 0.0)
let other = frame.sky.sample(dir, smp = point_clamp, level = 0.0)
```

**The functions are ordinary builtins, and the methods fall out of UFCS.**
A default argument reads the texture's default sampler, and a named argument overrides it:

```sgl sketch
@builtin fun sample(tex: texture_2d[T], coord: float2, smp: sampler = tex.default_sampler) -> T
@builtin fun sample(tex: texture_2d[T], coord: float2, smp: sampler = tex.default_sampler, .level: float) -> T
```

**Texels have explicit `load` and `store`, and a subscript is sugar on top.**
A subscript is variadic, and takes named and optional arguments like a call, so a level has a place: `t[xy]`, `t[xy, level = 2]`, `img[xy] = v`.

**The texture and sampler arguments are compile-time constants.**
A resource is not a runtime value on any target, so a function taking one is instantiated per resource, which every-call-inlines already implies ([inferred-comptime.md](inferred-comptime.md)).

## What it touches

* Nothing of the call model, which is [CHK-247](../semantics/checking.md#calls-and-overloads) and its neighbours.
* A default that reads another parameter's binding, `tex.default_sampler`.
* Generics over a texture's component type, `texture_2d[T] -> T`.
* The builtin registry: records whose parameters are resources, and a default reading another parameter.
* Bindings: `@sampler(name)` on a texture member, naming a static sampler at file scope or in the same binding ([bindings.md](../bindings.md#samplers)).

## Already fixed by the syntax

* Attributes attach to a binding member and take parsed arguments.
* A subscript is an index expression whose list may hold any argument, named ones included.
* `sampler` as a keyword denotes a type in a type position, so `smp: sampler = …` is a parameter like any other.

## What exists

`sample`, `load`, `store` and `size` are builtins of the prelude, for 2D shapes, called as methods through UFCS.
A sample takes its sampler as an argument, since no texture has a default one yet, and a named-only `level` picks the overload that samples at a level.
`load` and `size` default their level to 0.

## Open

* Whether `@sampler` may name a dynamic sampler member, and what the host then binds.
* What a texture without `@sampler` does when sampled without one: an error at the call, or at the declaration.
* The full set: gradients, bias, gathers, comparison, sizes, and what each is called.
