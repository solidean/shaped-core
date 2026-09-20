# SGL semantics

*Tracer: deliberately thin.*

The semantics of SGL start where the [syntax](../syntax/_index.md) ends: at the ASTs of the files of one module.
Back to the [specification](../_index.md).

This section is a **tracer**: one thin path through every phase, for one program, [cube.sgl](../../../tests/samples/cube.sgl).
It states what the compiler does today, and it is normative for exactly that much.
Whatever the cube does not need is the normal error `unsupported-yet`, which names the construct and never guesses a meaning.
The ideas the tracer follows are in the [incubator](../incubator/_index.md), and they stay there until this section grows into them.

```text
ASTs of one module -> check -> side tables over the ASTs
                            -> one flat typed tree per entry point -> legalize -> emit -> HLSL for dx12, HLSL for vulkan, WGSL, MSL
```

The flat tree has two forms.
The **structured form** is what the language means and what inlining will write; the **core form** is what a target prints.
Control flow exists in the tree before the source can say it: the check pass still writes `let` and `return` only, and the tests build the rest by hand.

## The files

| file | holds | rule ids |
|---|---|---|
| [checking.md](checking.md) | the check pass: symbols, types, overloads, bodies, entry points, the flat tree, and its diagnostic kinds | `CHK` |
| [why/checking.md](why/checking.md) | the reasons behind the rules of the check pass | |
| [emitting.md](emitting.md) | the emitters: targets, names, addresses, the inline binding, matrices, and the shape of the text | `EMIT` |
| [why/emitting.md](why/emitting.md) | the reasons behind the rules of the emitters | |
| [evaluation.md](evaluation.md) | what a flat tree means: the abstract machine over the structured form, evaluation order, effects, blocks and exits | `EVAL` |
| [why/evaluation.md](why/evaluation.md) | the reasons behind the rules of the machine | |
| [legalization.md](legalization.md) | an informative appendix: the core form, the rules that reach it and their cost, and what each core construct is in each target | `LEGAL` |
| [why/legalization.md](why/legalization.md) | the reasons behind the rules of the legalizer | |

## What is not here yet

* Modules beyond the one unnamed module, interfaces, and `use`.
* The inliner, and with it calls of functions of the program, generics, lambdas and nested functions.
* Control flow, assignment and `mut` in the SOURCE: the flat tree and the emitters carry them, and the check pass does not write them yet.
* Literal types, integer literals, conversions and `as`.
* Resources, samplers, enums, constants and type aliases.
* An emitter for GLSL, a Metal compiler for the MSL text, and a binding that is not `@inline`.
