# Automatic Differentiation

*Incubator: not normative.*

## The idea

**SGL differentiates a function for you**: `fwd_diff f` and `bwd_diff f` are functions the compiler writes from the body of `f`, so a training kernel runs on sg like any other.
The case that brought it here is training Gaussian splats, whose backward pass is pages of hand-derived chain rule in every implementation that exists.
The math changes with every variant tried (2D splats, other kernels, other camera models), and a hand-written adjoint has to be rederived each time.

**It is a transform of the flat tree, not of the AST.**
Every call is inlined before anything else reads a function ([function-model.md](function-model.md)), so there is no call graph to carry derivatives through.
`bwd_diff f` is: flatten `f`, then write its adjoint as more flat tree, in the structured form, before `legalize`.
The emitters never learn that a derivative exists.

**A builtin's derivative lives in its registry record**, beside its evaluator and its spelling per target.
Where SGL can say the rule, it is SGL in the prelude, as `@derivative(of = sin)` on an ordinary function; a C++ rule is for what it cannot.

**The interpreter is the gradient check.**
A corpus `test` holds `bwd_diff f` against `fwd_diff f`, and both against central differences on the interpreter.
The randomized differential test of the legalizer can hold the transform to the same, over generated trees.
That is the strongest argument for doing this in SGL at all: a hand-written backward kernel has none of it.

```sgl sketch
// differentiable in every float parameter, and the compiler refuses what it cannot differentiate
@differentiable fun splat_cov2(scale: vec3, rotation: quat, view: mat3, j: mat2x3) =>
    let m = rotation_matrix(rotation) * diag(scale)
    j * view * m * transpose(m) * transpose(view) * transpose(j)

// forward: the value and its directional derivative
let (cov, d_cov) = fwd_diff(splat_cov2)(s, ds, q, dq, view, no_diff, j, no_diff)

// reverse: the adjoint of each differentiable parameter, from the adjoint of the result
let grads = bwd_diff(splat_cov2)(s, q, view, j, d_cov_out)

// what the transform must not look through
let tile = no_diff tile_of(center)

// a hand-written adjoint where the derived one is wrong or wasteful
@derivative(of = composite_tile)
fun composite_tile_bwd(...)

test bwd_diff(splat_cov2)(...) ~= central_difference(splat_cov2, ...)
```

**Where it pays and where it does not**, for the case that brought it here:

* The per-splat preprocessing (covariance from scale and rotation, the projection to 2D, spherical harmonics) is straight-line math and the derived adjoint is exactly right.
* The per-tile compositing is a front-to-back loop of data-dependent length.
  A derived reverse pass would keep every step's state, where the reference implementation walks back to front and divides the transmittance back out.
  That loop gets a `@backward` written by hand, and the per-step math inside it is still derived.
* An SSIM loss is a convolution, which differentiates without help.

**Differentiable rendering is the other case it is for**, and it asks two things more of the design.
A trace is no differentiable operation: visibility is `no_diff`, and what is shaded behind the hit is derived.
A path's loop has no bound, so it can keep no tape, and its derivative is written by hand in the manner of path replay: the path traced again, the adjoint carried along it.
So a derivative written by hand is part of the first design, not an extension of it, and nothing may assume that every loop is bounded.

## The prototype

A prototype is built: [flatten_diff.cc](../../../src/shaped-graphics-language/check/flatten_diff.cc) and the files it names.
Its tests are [corpus/derivatives/](../../../tests/corpus/derivatives/).
They hold both modes to central differences, among them the whole gradient of one Gaussian splat's opacity at a pixel, whose backward kernel every target writes.
[autodiff-fuzz-test.cc](../../../tests/check/autodiff-fuzz-test.cc) writes random programs over every form the prototype takes, and holds each function's two modes to each other.
They must agree even beside a kink, and both match a central difference wherever one-sided and central differences say the function is smooth there.

* **The surface is two prelude names**, and a call that names a function resolves against an overload the check pass writes for it.
  `fwd_diff(f, a, da, b, db)` gives the tangent of the result, and `bwd_diff(f, a, mut da, b, mut db, dr)` adds each adjoint to the caller's place.
  A parameter of no differentiable type takes no tangent, and neither does one marked `@no_diff`: `fwd_diff(scaled, x, dx, n)` for an int `n`.
  Adding rather than returning settled the first open question below: it is what accumulating a gradient wants, and a test starts its places at zero.
  The flattener writes the derivative where the call stands, as it does for `by_target`.
* **One rule per builtin serves both modes.**
  `derive` writes a tangent as linear operations over primal values, and reverse mode **transposes** that linear expression, as JAX linearizes and then transposes.
  So a new builtin needs only its forward rule, and only the linear primitives have a transpose: `+`, `-`, `*`, `/` by a value, `dot`, `cross`, `transpose`, `select`, constructions and fields.
* **Reverse mode runs on straight structured code.**
  Every declaration is hoisted, every early leave becomes a flag that guards what follows it, and every condition is recorded.
  The backward sweep restores what each statement overwrote from a save, and transposes that statement's tangent.
* **A loop of constant bounds stays a loop, over a tape.**
  Inside it, each save and each recorded condition is a local array with one slot per iteration, of every kept loop around it.
  The backward sweep walks the loop down from its last iteration, and every read of its index in what it clones reads the index reversed.
  [loops.sgl](../../../tests/corpus/derivatives/loops.sgl) has one of 64 iterations, whose backward kernel is 355 lines of HLSL and 275 DXIL instructions, against 5508 and 2309 unrolled.
  The same loop of 4 iterations is 117 kept against 149 unrolled, so no trip count below which to unroll pays: what decides is the body.
  A loop whose body is a few operations is the exception, 57 kept against 24 unrolled, and `@unroll` on it unrolls it going backward too.
  A `continue` leaves a block around the body, and a `break` or a `return` from inside the loop sets a flag.
  The iterations after it run as nothing, each one's guard recorded on the tape, so going backward they are skipped alike.
  A tape holds at most 1024 slots, and a loop of any other bound is `unsupported-yet`.
  Forward mode follows every loop.
* **A derivative written by hand is two attributes.**
  `@forward(f)` stands on a function of each parameter and its tangent, which gives the result's tangent.
  `@backward(f)` stands on a function of each parameter and its adjoint as `mut`, then the result's adjoint.
  Inside a function being differentiated, a call of `f` binds its arguments to locals and runs as it is, and its derivative is the written one.
  Going backward `f` runs forward and is never reversed, so it may hold a loop of any bound.
  [written-derivatives.sgl](../../../tests/corpus/derivatives/written-derivatives.sgl) composites front to back until almost nothing shows through, and walks back without a tape.
  The attribute names `f` by its text for now, which a resolved expression would replace.
* **A position's tangent is its direction**: a `pos3`'s is a `vec3`, an `hpos4`'s a `float4`, and `mat4 * pos3` moves a change of the position without the translation.
  A struct's tangent is the struct itself for now, so a position in one holds its direction, which every target writes alike.

## Buffers and textures

A trainer's parameters and a renderer's scene live in buffers and textures, so what a derivative does with a read decides whether a gradient arrives at all.
A read whose gradient went nowhere would be a silent wrong answer, the one failure a derivative must not have.

What the prototype does, in [buffers.sgl](../../../tests/corpus/derivatives/buffers.sgl):

* A constant block's member is a constant: a camera, a setting, what the host hands every invocation alike.
* **A buffer is differentiable by its attributes**: `@gradient(training.gradients) @tangent(training.tangents) weights: buffer[float]`.
  Going backward, a read's adjoint is added to the `@gradient`'s element at the read's index, a `mut buffer[atomic[float]]`.
  Going forward, a read changes by the `@tangent`'s element, a `buffer[float]`; without one it holds still.
  The pairs are buffers the program declares and the host binds, so neither sg nor slib knows a derivative exists.
  The entry point lists the paired bindings, since the derivative reaches what the function never named.
  [autodiff-buffer-test.cc](../../../tests/interpret/autodiff-buffer-test.cc) runs both on buffers a driver binds.
* A read of any other buffer is `unsupported-yet`, and the diagnostic says what to do instead.
  So is a read of a buffer of anything but floats, whose gradient needs an atomic per component.
* `no_diff(x)` reads anything as a constant, which is the opt-out at an expression where `@no_diff` is the one at a parameter.
* A function that reads an element can carry its own derivatives instead: `@forward` reads the tangent, and `@backward` adds the adjoint.
* **An adjoint from many invocations is a float atomic add**: `atomic[float].add`, a compare-exchange loop on the bits on every target ([bindings.md](../bindings.md#atomics)).
  It is the device's own add where it has one, vulkan with VK_EXT_shader_atomic_float and metal.

What is left of a differentiable buffer:

* **A pairing is matched by its text**, as `@forward(f)` is, until an attribute's argument is an expression the check pass resolves.
* **A buffer of vectors or structs**, whose gradient is an atomic per float leaf.
* **An index every invocation of a subgroup shares is reduced first**, one subgroup sum and one atomic in place of thirty-two.
  A tiled splat rasterizer is that case: every pixel of a tile walks the same splats at once, which is the optimization 3DGS's CUDA backward relies on.
* **A texture's sample scatters its adjoint to the texels it filtered**, four atomic adds at the bilinear weights, one level for a sample at a level.
  A texture that is trained is an image with a gradient image beside it.

What it found in SGL itself:

* The emitted text is long, about 800 lines of HLSL for the splat's backward kernel, and DXC folds it to a DXIL of about the primal's size.
  A save of a local that is assigned once, and a flag that nothing reads after it, are what to cut first.

## What it touches

* **Types**: which types are differentiable, and each one's **tangent type**.
  `float`, the float vectors and matrices and `half` are their own.
  The geometry types are not: the tangent of a `pos3` is a `vec3`, and of an `hpos4` a `vec4`, which typed geometry gives for free where other languages spell it out.
  A struct is differentiable member-wise, its integer and `no_diff` members carried as they are.
* **The check pass**: `@differentiable`, `no_diff`, `@forward(f)` and `@backward(f)`, and a diagnostic for each construct the transform refuses.
  A call inside a differentiable body that reaches a non-differentiable builtin is a diagnostic, never a zero derivative.
* **The flattener or a pass after it**: the forward transform (a dual value per node) and the reverse transform (the primal, then the adjoints in reverse).
* **Control flow**: an `if` records its condition for the reverse pass.
  A loop with a constant bound keeps a local tape sized by the bound.
  Any other loop is refused unless its function has a `@backward`.
* **The builtin registry**: a derivative rule per differentiable record, and `prelude/builtins.sgl` regenerated with them.
* **Atomics**: an adjoint added from many invocations, which [Buffers and textures](#buffers-and-textures) describes.
* **Testing**: `central_difference` or its like in the prelude, and an approximate comparison a `test` can state.

## Already fixed by the syntax

* A parameter of function type takes a function's name ([CHK-317](../semantics/checking.md#calls-and-overloads)).
  So `fwd_diff f` and `bwd_diff f` are ordinary calls of compile-time functions, not new syntax.
* Every function inlines and none recurses, so the transform always sees the whole body.
* Attributes are an open set ([stage-interfaces.md](stage-interfaces.md)), so `@differentiable`, `@forward` and `@backward` need no grammar.
* `mut` parameters exist ([CHK-315](../semantics/checking.md#functions)), which is one way the reverse pass could hand back adjoints.

## The order to build it in

The prototype has done forward mode, reverse mode, loops of constant bounds over a tape, derivatives written by hand, `@no_diff`, and the tangent types of the builtin geometry types.
It has buffers of floats differentiable by their `@gradient` and `@tangent` too.

1. A loop whose bound is stated rather than constant, as Slang's `[MaxIters]` is, which a `while` needs.
2. A struct's tangent type, a struct of its fields' tangents, so a struct holding a position says what it holds.
3. The rules as registry records.
4. A subgroup reduction ahead of a gradient's atomic where the index is uniform, and buffers of vectors and structs.

## Open

* **Whether a call may mark a parameter it does not differentiate**, beside `@no_diff` on the declaration, which the prototype has.
* **Whether a tape may spill** to a buffer for a long bounded loop, or the bound is limited to what fits in registers and the rest needs a `@backward`.
* **Accumulation across invocations**: a float atomic add behind a feature, a compare-exchange loop on the bits as a polyfill, or a subgroup reduction first and one atomic per subgroup.
  The last is what makes any of them fast, and it may want to be a prelude function rather than something the transform writes.
* **User-declared tangent types**, which wait for interfaces.
* **Higher-order derivatives** (`fwd_diff` of a `bwd_diff`), which nothing has asked for yet.
* **Prior art**: Slang has the same surface (`fwd_diff`, `bwd_diff`, `no_diff`, custom derivatives), and its choices on each open question are worth reading before deciding.
