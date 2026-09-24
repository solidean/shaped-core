# Feature Levels

*Incubator: not normative.*

## The idea

**Today SGL stays on the intersection of all backends, and that cannot last.**
Ray tracing, binding arrays, tier 2 formats and their like are not fully portable, and they are needed.

**A function says which features it needs**, and checking refuses every use of a feature that was not declared.
That refusal is a normal error, so it works during development and not only when a shader reaches a device that lacks the feature.

This may well be an effect per function, similar to bindings ([binding-effects.md](binding-effects.md)).
`@stages` is the precedent that exists: a function names the stages it may be reached from, and the entry point is judged once everything is inlined ([CHK-188](../semantics/checking.md#entry-points)).
A function that uses a feature declares it, a function that calls it must declare it too, and the entry point states what the pipeline asks of the device.

```sgl sketch
@requires(ray_tracing) fun trace_shadow(origin: pos3, dir: vec3){scene} -> float

@pixel @requires(ray_tracing, binding_arrays) fun lit_ps(p: pixel_input){scene, materials} -> target
```

The spelling above is a placeholder: an attribute is the form that needs no new syntax.

## What it touches

* The check pass: a feature set per function, propagated through calls like a binding requirement.
* The prelude: a `@builtin` declaration names the feature it belongs to, so using it is what demands the feature.
* The emitters: a target that lacks a feature refuses the entry point, with the feature's name.
* `sg`: the feature names should be the ones `sg` reports for a device, so the host can ask before it builds a pipeline.
* Reflection and host code generation ([host-code-generation.md](host-code-generation.md)): an entry point's feature set is part of what the host is told.

## Already fixed by the syntax

* Attributes are an open set, attach to any declaration, and take parsed arguments.
* A function's binding list shows that a per-function effect has a place in the signature, should features want one of their own.

## Open

* Whether it is an attribute, a list in the signature, or a declaration at module level.
* Whether a function must restate the features of its callees, as it may have to for bindings, or whether they are inferred below the entry point.
* What the feature names are, and whether they are levels, single features, or both.
* Whether code may branch on a feature at compile time, and what then keeps both arms checked.
* Whether a module can declare a feature for everything in it.
