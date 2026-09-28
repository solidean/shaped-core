# Uniformity Analysis

*Incubator: not normative.*

## The idea

**SGL judges uniformity itself, and refuses a sample that is not uniform enough, on every target alike.**
A sample that picks its own level takes screen-space derivatives, and a derivative is only defined where the neighbouring pixels of a quad took the same path.
HLSL takes such a sample in divergent control flow and gives an undefined result, while WGSL refuses it outright.
SGL should refuse it once, for every target, and say why.

**The analysis runs late, probably after everything is inlined.**
Every function inlines ([function-model.md](function-model.md)), so the entry point's flat tree is the whole program, and the question is asked once, of that tree.
It follows where a value may differ between invocations — a stage input, a storage read, a thread id — through every branch and loop condition it reaches.

**The diagnostic says how to fix it.**
Either the control flow is annotated as uniform, with `@uniform` or something like it, where the author knows more than the analysis can prove.
Or the gradient is computed before the branch, where every pixel of the quad still runs, and the sample inside takes it explicitly through the `grad` form of `sample`.

**It replaced a stopgap.**
The WGSL text of an entry point that sampled used to open with `diagnostic(off, derivative_uniformity);`, which kept Tint quiet and left the undefined result in place.

## What it touched

* Checking: a pass over the core tree of each entry point, and the diagnostic kind `non-uniform-control-flow` for a call in non-uniform control flow.
* The builtin registry: a record says whether its builtin takes derivatives implicitly or is a barrier, and the pass reads that alone.
* Texture methods: `sample(…, grad_x = …, grad_y = …)` takes explicit gradients, and `ddx` and `ddy` compute them ([texture-methods.md](texture-methods.md)).
* Emitting: the WGSL directive is gone.
* Syntax is untouched: an annotation that asserts a branch uniform is still open.

## Already fixed by the syntax

* Attributes attach to declarations and take parsed arguments, so a statement attribute would be a new place for one rather than a new form.
* `@stages` already keeps an implicit-derivative sample out of every stage but the pixel stage.

## What exists

The pass is CHK-282 to CHK-284, over the core tree of each entry point, for barriers and for every builtin that takes derivatives.
It judges an index into a binding array too, which is marked `nonuniform` or proven uniform (CHK-300).
Its rules are WGSL's, so Tint accepts what it accepts, and it is coarser than WGSL's in one place:
a local set anywhere in non-uniform control flow is non-uniform everywhere, where WGSL follows each assignment.

## Open

* Whether a finer analysis proves more programs uniform than WGSL's rules, and whether every target then still accepts them.
* Where `@uniform` stands: on the branch, on the value it tests, or on a function's parameter.
* Whether an annotated branch is trusted, or checked at run time in a debug build.
* Subgroup operations, which need the same judgement once they exist.
