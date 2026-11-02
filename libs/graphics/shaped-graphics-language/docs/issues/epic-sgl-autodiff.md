---
created: 2026-11-02
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/docs/architecture.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/generics.md
---

# Automatic differentiation, toward differentiable rendering

Differentiable rendering is a use case we want to reach eventually: inverse rendering, material and lighting fits, neural components trained inside a renderer.
That wants SGL to produce the derivative of a function from its body, forward and reverse, instead of a hand-written gradient beside every shader.

Full inlining is what makes this tractable in SGL: every call site sees the whole body, there are no function pointers, and a generic is already one checked instance per use.
So differentiation can be a source-to-source pass on the flat tree, like the other passes, and its output stays readable text for every target.

What a design has to settle, none of it decided:

- **Spelling.** How a function asks for its derivative (`fwd_diff(f)`, `bwd_diff(f)`, or an attribute on the function).
  And how a caller supplies its own derivative for a function the pass should not look into.
- **Forward mode first.** Dual numbers through the body are the smaller step, and enough for few parameters; likely the first child issue.
- **Reverse mode.** The adjoint needs the primal values back in reverse order.
  A loop with a trip count known only at run time is the hard case: store per iteration, recompute, or checkpoint.
- **What is not differentiable.** Integers, `discard`, comparisons and branches (piecewise, derivative of the taken side), and how that is reported rather than guessed.
- **GPU memory.** The adjoint of a read is a write: a gather becomes a scatter, which needs atomic adds into gradient buffers, and float atomics are not equally available on every target.
  Portability by default applies here as everywhere else.
- **Generics.** Which types are differentiable, and whether that rides on the existing per-instance checking rather than on interfaces.

Out of scope at the language level: visibility and edge discontinuities in a trace (reparameterization, path replay).
Those are a renderer's techniques built on top, in sv or sr.

Prior art worth reading before designing: Slang's autodiff (`[Differentiable]`, `fwd_diff` / `bwd_diff`, custom derivatives), Dr.Jit as used by Mitsuba 3, and Enzyme on LLVM IR.

Each piece becomes a child issue when it is taken up.
