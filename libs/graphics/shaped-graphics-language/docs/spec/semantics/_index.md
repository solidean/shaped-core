# SGL semantics

*Tracer: deliberately thin.*

The semantics of SGL start where the [syntax](../syntax/_index.md) ends: at the ASTs of the files of one module.
Back to the [specification](../_index.md).

This section is a **tracer**: one thin path through every phase, for two programs, [cube.sgl](../../../tests/samples/cube.sgl) and [helpers.sgl](../../../tests/samples/helpers.sgl).
It states what the compiler does today, and it is normative for exactly that much.
Whatever the two do not need is the normal error `unsupported-yet`, which names the construct and never guesses a meaning.
The ideas the tracer follows are in the [incubator](../incubator/_index.md), and they stay there until this section grows into them.

```text
ASTs of one module -> check -> side tables over the ASTs
                            -> one flat typed tree per entry point -> legalize -> emit -> HLSL for dx12, HLSL for vulkan, WGSL, MSL
```

The flat tree has two forms.
The **structured form** is what the language means and what the check pass writes; the **core form** is what a target prints.
A call of a function of the program is inlined into the structured form, so no later phase knows what a function is.

## The files

| file | holds | rule ids |
|---|---|---|
| [checking.md](checking.md) | the check pass: symbols, types, overloads, bodies, control flow, returning, calls, entry points, the flat tree, and its diagnostic kinds | `CHK` |
| [why/checking.md](why/checking.md) | the reasons behind the rules of the check pass | |
| [emitting.md](emitting.md) | the emitters: targets, names, addresses, the inline binding, matrices, and the shape of the text | `EMIT` |
| [why/emitting.md](why/emitting.md) | the reasons behind the rules of the emitters | |
| [evaluation.md](evaluation.md) | what a flat tree means: the abstract machine over the structured form, evaluation order, effects, blocks and exits, and which tree a call and a statement of the source are | `EVAL` |
| [why/evaluation.md](why/evaluation.md) | the reasons behind the rules of the machine | |
| [legalization.md](legalization.md) | an informative appendix: the core form, the rules that reach it and their cost, and what each core construct is in each target | `LEGAL` |
| [why/legalization.md](why/legalization.md) | the reasons behind the rules of the legalizer | |

## What is not here yet

* Modules beyond the one unnamed module, interfaces, and `use`.
* Generics, lambdas, function values, nested functions, methods and `mut` parameters, each of which the inliner has yet to carry.
* A `for` over anything but an `int` range, and a `let` without a value.
* Literal types, conversions and `as`, and `true` and `false`.
* Resources, samplers, constants and type aliases.
* An emitter for GLSL, a Metal compiler for the MSL text, and a binding member that is a texture or a sampler.
