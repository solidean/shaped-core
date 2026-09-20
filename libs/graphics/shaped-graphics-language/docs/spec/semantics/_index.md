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
                            -> one flat typed tree per entry point -> emitters
```

## The files

| file | holds | rule ids |
|---|---|---|
| [checking.md](checking.md) | the check pass: symbols, types, overloads, bodies, entry points, the flat tree, and its diagnostic kinds | `CHK` |
| [why/checking.md](why/checking.md) | the reasons behind the rules of the check pass | |

## What is not here yet

* Modules beyond the one unnamed module, interfaces, and `use`.
* The inliner, and with it calls of functions of the program, generics, lambdas and nested functions.
* Control flow, assignment and `mut`.
* Literal types, integers, conversions and `as`.
* Resources, samplers, enums, constants and type aliases.
* The emitters, under `targets/`.
