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
let base = frame.sky.sample_level(dir, 0.0)
let other = frame.sky.sample(dir, sampler = point_clamp, level = 0.0)
```

**The functions are ordinary builtins, and the methods fall out of UFCS.**
A default argument reads the texture's default sampler, and a named argument overrides it:

```sgl sketch
@builtin fun sample(tex: texture2d[T], coord: float2, smp: sampler = tex.default_sampler) -> T
@builtin fun sample(tex: texture2d[T], coord: float2, level: float, smp: sampler = tex.default_sampler) -> T
```

`level` is probably keyword-only, so `sample(uv, 0.0)` cannot be misread as a sampler or a bias.
The free spelling `sample(sky, dir)` stays valid, since UFCS is sugar over it.

**Texels have explicit `load` and `store`, and a subscript is sugar on top.**
A subscript is variadic, and takes named and optional arguments like a call, so a level has a place: `t[xy]`, `t[xy, level = 2]`, `img[xy] = v`.

**The texture and sampler arguments are compile-time constants.**
A resource is not a runtime value on any target, so a function taking one is instantiated per resource, which every-call-inlines already implies ([inferred-comptime.md](inferred-comptime.md)).

## What it touches

* Methods and UFCS: a call `x.f(a)` resolving to a free `f(x, a)` ([members-and-properties.md](members-and-properties.md)).
* Default and named arguments, and keyword-only parameters.
* Generics over a texture's component type, `texture2d[T] -> T`.
* The builtin registry: records whose parameters are resources, and a default reading another parameter.
* Bindings: `@sampler(name)` on a texture member, naming a static sampler at file scope or in the same binding ([bindings.md](../bindings.md#samplers)).

## Already fixed by the syntax

* Attributes attach to a binding member and take parsed arguments.
* A subscript is an index expression whose list may hold any argument, named ones included.
* `sampler` as a keyword denotes a type in a type position, so `smp: sampler = …` is a parameter like any other.

## Until then

The first texture functions are stubs named `DEBUG_…`, which is SGL's marker for an in-progress stand-in.
They are free functions in the argument order the methods will have — texture, coordinate, level, sampler — so replacing them is a rename.

## Open

* Whether `@sampler` may name a dynamic sampler member, and what the host then binds.
* What a texture without `@sampler` does when sampled without one: an error at the call, or at the declaration.
* Whether `sample` and `sample_level` are one function with an optional `level`, since a pixel stage alone may leave it out.
* The full set: gradients, bias, gathers, comparison, sizes, and what each is called.
