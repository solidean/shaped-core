---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/raytracing-futures.md
---

# Inline target code, as inline assembly is to a compiler

The portable language is the default, and every target feature SGL has not named yet is out of reach until it does: SER, opacity micromaps, NVAPI intrinsics, a Metal-only builtin.
An escape hatch would let a function's body be written in a target's own text, as inline assembly lets C reach an instruction the compiler does not know.
SGL cannot check such a body, so the contract around it is what makes it safe; nothing below is decided.

**A function with an SGL signature and one body per target.**
Callers see an ordinary SGL function: its parameters, its result and their marks (`mut`, `out`) are checked like any other.
Each target that has it gets a text body, and a target without one is `target-lacks-feature` where an entry point reaches the call.
A feature grant or `if feature(...)` is then how a portable path falls back where a target lacks the body ([epic-sgl-feature-grants](epic-sgl-feature-grants.md)).

**The signature contract.**
How each SGL type of the signature is spelled in each target's text has to be specified and stable: vectors, matrices, structs, resources, a ray or a hit.
The body names its parameters through placeholders, as inline assembly names its operands, rather than through names the emitter might mint differently.

**Hazards, declared rather than inferred.**
The footprint has to cover everything the emitted text uses ([bindings.md "Footprint"](../spec/bindings.md)), and SGL cannot read a target body.
So a body reaches GPU memory only through resource parameters, whose marks state read or write, as a clobber list states what inline assembly touches.
Then the footprint stays exact with no help from the body.

**Effects and uniformity, declared.**

- Whether the body is pure, so flatten may fold, merge or move the call, or has effects, so it stays where and as often as written.
- Whether the body needs uniform control flow (a wave operation, a derivative, a barrier), which WGSL's uniformity analysis and the check pass both have to know.
- Whether its result is uniform.

**What a body may also need.**
A preamble per target, deduplicated across calls: an `#include`, an `enable` directive, an extension, a helper function.
And error mapping: the target compiler's error inside a body should point back at the SGL source.

**Hot reload keeps working**, since a body is text that goes through the same path as everything else.
