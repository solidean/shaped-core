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

**Built, and normative in [semantics/differentiation.md](../spec/semantics/differentiation.md)**:
forward and reverse mode over floats, vectors, matrices, geometry types and structs, loops of constant or stated bounds, derivatives written by hand,
and buffers and textures differentiable by their `@gradient` and `@tangent`.
The design and its reasons are [incubator/autodiff.md](../spec/incubator/autodiff.md); sg fits sixteen 2D Gaussians to an image with it on every backend.

What is left, each a child issue when it is taken up:

- **The forward-only builtins going backward**: `ddx`, `ddy`, `fwidth`, `quad_broadcast`, `subgroup_broadcast` and the shuffles, whose transposes need which lane each invocation is.
- **A texture's levels past 0**, and a sample through a sampler bound at run time.
- **`tangent[S](…)` built from its fields** for a minted tangent; a declared one (DIFF-37) builds as any struct.
- **Higher-order derivatives**, which nothing has asked for yet.

## Compared with Slang (2026-11-13)

Slang's autodiff is the prior art closest to SGL's: the same `fwd_diff` and `bwd_diff`, `no_diff`, and derivatives written by hand.
What follows was read from its user guide, its SLANG.D paper and the SlangPy docs; what could not be confirmed is marked so.

**Where SGL is ahead**:

- **Memory is differentiable by declaration.**
  A buffer's or a `texture_2d`'s `@gradient` and `@tangent` scatter adjoints by atomics, bilinear samples and gathers included.
  In Slang a read's adjoint is the user's to write, by a custom derivative or a `DiffTensorView`, and every texture example found writes it by hand.
- **A gradient's atomics are shared through the subgroup** where the entry point declares `subgroups`, which Slang leaves to the user.
- **A position's change is a direction** by type, where Slang's positions are plain vectors.
- **Every builtin states its derivative** in the registry, a test refuses one that does not, and a test holds every rule to a central difference.
  The rules are normative ([DIFF](../spec/semantics/differentiation.md)); Slang has a user guide and a paper.
- **A `test` is a gradient check**, on the interpreter.

**Where Slang is ahead**, in the order worth closing:

1. **Differentiable generic and interface code** (generics and declared tangents now done, DIFF-4 and DIFF-37): `IDifferentiable`, a type's own `Differential`, differentiable member functions.
   SGL has no interfaces by design ([generics.md](../spec/incubator/generics.md)): a generic body is checked per instance.
   So differentiating a generic is differentiating its instance, and `tangent[T]` of a type parameter resolves per instance as `scalar_of[V]` does.
   A type declaring its own tangent has no interface to declare it against, which is a design question of its own.
2. **Loops past what a tape holds** (done: checkpointed in segments, DIFF-21).
   Slang bounds a loop with `[MaxIters]`, and its paper describes choosing between storing and recomputing; SGL stores every iteration in a tape of at most 1024 slots.
3. **Higher-order derivatives**, `fwd_diff` of a `bwd_diff`, which Slang has and some differentiable rendering needs.
4. **The host side**: SlangPy injects kernels into PyTorch's autograd, so a training loop is Python.
   SGL's is the host's own, through sg; this is a project beside the language, not in it.
5. **Use**: Slang's autodiff is in published research and in production neural shading; SGL's has its tests and sg's splat fit.

**Not measured**: how fast SGL's backward kernels run beside Slang's.
SGL's emitted text is long, about 800 lines of HLSL for a splat's backward kernel, which DXC folds to about the primal's size.

Sources:
[Slang user guide: Automatic Differentiation](https://shader-slanggithubio-aidanfnv-wip.readthedocs.io/en/latest/external/slang/docs/user-guide/07-autodiff.html),
[SLANG.D](https://research.nvidia.com/labs/rtr/publication/bangaru2023slangd/bangaru2023slangd.pdf),
[custom derivatives](https://shader-slanggithubio-aidanfnv-wip.readthedocs.io/en/latest/autodiff-tips-custom-diffs.html),
[DiffTensorView](https://shader-slanggithubio-aidanfnv-wip.readthedocs.io/en/latest/external/slang/docs/deprecated/a1-02-slangpy.html),
[SlangPy autodiff](https://nv-sgl.readthedocs.io/en/latest/src/autodiff/autodiff.html),
[SlangPy and PyTorch](https://nv-sgl.readthedocs.io/en/latest/src/autodiff/pytorch.html),
[neural shading](https://developer.nvidia.com/blog/how-to-get-started-with-neural-shading-for-your-game-or-application/).

Out of scope at the language level: visibility and edge discontinuities in a trace (reparameterization, path replay).
Those are a renderer's techniques built on top, in sv or sr.
