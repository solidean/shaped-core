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

- **Builtins stated not yet**: `determinant`, `refract`, the gathers, `ddx` and `ddy`, and the subgroup and quad operations.
- **A texture's levels past 0**, and a sample through a sampler bound at run time.
- **`tangent[S](…)` built from its fields**, user-declared tangent types, and `tangent[T]` of a type parameter.
- **A tape past 1024 slots**, which would spill to memory or ask for a `@backward`.
- **Higher-order derivatives**, which nothing has asked for yet.

Out of scope at the language level: visibility and edge discontinuities in a trace (reparameterization, path replay).
Those are a renderer's techniques built on top, in sv or sr.
