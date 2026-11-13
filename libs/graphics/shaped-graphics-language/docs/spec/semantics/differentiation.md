# Differentiation

*Tracer: what the compiler does today, normative for that much.*

SGL writes the derivative of a function from its body, in two modes.
Forward mode carries a change of the arguments to a change of the result, and reverse mode an adjoint of the result back to adjoints of the arguments.
Both are written into the flat tree where the call stands, as structured code every target writes, so no target and no emitter knows a derivative exists.
Back to the [semantics](_index.md); the design and its reasons are in [incubator/autodiff.md](../incubator/autodiff.md).

Every construct below that a derivative cannot follow is `unsupported-yet` at the call that asked for the derivative, naming the construct.
A derivative is never silently zero where the function changes.

## The calls

* **DIFF-1** `fwd_diff(f, a, da, b, db, …)` gives the change of `f`'s result when each argument changes by its tangent: each argument that takes a tangent (DIFF-3) is followed by it.
* **DIFF-2** `bwd_diff(f, a, mut da, b, mut db, …, dr)` adds to each tangent place the adjoint of its argument for the adjoint `dr` of the result, and gives nothing.
  It adds and does not assign, so places that start at zero hold the gradient, and a second call adds a second one.
* **DIFF-3** A parameter takes a tangent where its type is differentiable (DIFF-6), it is no `mut` parameter, and it carries no `@no_diff`.
  Any other parameter is passed alone: `fwd_diff(scaled, x, dx, n)` for an `int` `n`.
* **DIFF-4** `f` names a function of the program that gives a value.
  A generic's derivative is its instance's: each value binds its parameter's type as a call of `f` would (CHK-340), and a value that takes a tangent (DIFF-3) is followed by it.
  A call whose values bind no instance, or that names anything but a function, matches no overload.
* **DIFF-5** A test runs both calls on the interpreter, which is how a derivative is held to a central difference.
  `nearly_equal(fwd_diff(f, x, d), (f(x + d * h) - f(x - d * h)) / (2.0 * h), within = …)` is that check.

## Types

* **DIFF-6** A type is differentiable where it holds a float or a half: a float or half scalar, vector or matrix, a position, a direction, an `affine3`, and an array or a struct that holds one.
* **DIFF-7** `tangent[T]` is the type of a change of a value of `T`.
  A position's is its direction: `tangent[pos3]` is `vec3`, and `tangent[hpos4]` is `float4`.
  A float, a vector, a matrix and an `affine3` are their own.
  An array's is an array of its element's tangents.
* **DIFF-8** A struct's tangent is a struct of its differentiable fields' tangents, in their order and under their names, and with no field for any other.
  So `tangent[splat]` of `at: pos3`, `weight: float` and `count: int` holds `at: vec3` and `weight: float`.
  A struct whose fields are all their own tangents is its own; any other struct's tangent is a type of its own, which a target writes beside the struct.
* **DIFF-9** `tangent[T]()` is the zero of `T`'s tangent, and a direction is that zero with its fields assigned.
  `tangent[T](…)` with arguments is `unsupported-yet`.
  `tangent[T]` of a type parameter is what each instance makes of it, as `scalar_of[V]` is (CHK-423).
  `tangent[T]` of a type that holds no float is `wrong-kind-of-name`.

## Holding values constant

* **DIFF-10** `no_diff(x)` is `x`, whose change is zero in both modes.
* **DIFF-11** `@no_diff` on a parameter holds it constant: the parameter takes no tangent (DIFF-3).
* **DIFF-12** A member of a constant block, the inline binding included, is a constant.

## Builtins

* **DIFF-13** Each builtin that gives a float and takes one states how a derivative passes through it, for every overload.
  [register_derivatives.cc](../../../src/shaped-graphics-language/builtins/register_derivatives.cc) states it:
  * **linear**: its change is the call of its arguments' changes, as for `+`, `-`, a construction, a conversion between float widths, `transpose`, `select` and a matrix's row or column;
  * **a rule**: the chain rule through it, as for `*`, `/`, `dot`, `cross`, `length`, `normalize` and the transcendentals;
  * **zero**: constant between the points where it jumps, so its change is zero everywhere: `floor`, `ceil`, `round`, `trunc`, `sign` and `step`;
  * **none**: its value is no function of its arguments a derivative follows, such as an atomic's update;
  * **forward only**: a rule going forward, whose transpose needs which lane of the quad or the subgroup each invocation is,
    as for `ddx` and `ddy` in each of their forms, `fwidth`, `quad_broadcast`, `subgroup_broadcast` and every shuffle but `subgroup_shuffle_xor`;
  * **not yet**: differentiable, with no rule written, which no builtin is today and one added later may be.
* **DIFF-14** A call of a builtin stated none or not yet, inside a function a derivative is taken of, is `unsupported-yet` and names the builtin.
  So is one stated forward only, in a function reverse mode is taken of.
* **DIFF-15** Where a builtin has a kink, its change is the side the builtin states:
  * `min` and `max` follow the argument they give, the first at a tie;
  * `clamp` follows `x` from its low bound to its high one, both included, and the bound it gives outside;
  * `abs` follows `sign(x)`, so its change at 0 is zero;
  * `saturate` follows `x` from 0 to 1, both included.
* **DIFF-16** Where a rule's formula has no finite value, as `sqrt` at 0 and `pow` at a base of 0 do, the change is what the formula gives.
* **DIFF-36** A subgroup or quad operation's change crosses the invocations that reach it.
  Going forward an invocation's change is what its result takes of every invocation's change, and going backward its adjoint is what every invocation's result took of its argument.
  So the two modes meet in their sums over the subgroup, which a test states with `subgroup_add`, and not invocation by invocation.
  `subgroup_min` and `subgroup_max` follow the invocations that hold the extreme, shared evenly where several do.
  The transposes stand wherever the operation does, over the invocations that reach it, as DIFF-34's do.
* **DIFF-17** A position's change moves through a transform as a direction: `m * p` changes by `dm * (p, 1) + m * (dp, 0)`, for a `mat4` and an `affine3` alike.

## Control flow

* **DIFF-18** An `if` or a `case` follows the branch it takes; its condition and its scrutinee have no derivative.
  So a derivative at a point where the branch taken changes is the derivative of the branch taken there.
* **DIFF-19** A `break`, a `continue` and a `return` leave as they do without the derivative, and what they skip contributes nothing.
* **DIFF-20** Forward mode follows every loop.
* **DIFF-21** Reverse mode follows a `for` over constant bounds, and a loop with `@max_iterations(n)` (CHK-528), which is a `for` over `0 ..< n` by EVAL-147.
  It keeps what each iteration overwrites and decides on a tape, one slot per iteration of every such loop around it, and walks the loop back from its last iteration.
  Loops whose iterations between them pass 1024 slots are `unsupported-yet`.
* **DIFF-22** Any other loop is `unsupported-yet` in reverse mode, unless a function around it has a derivative written by hand (DIFF-24).
* **DIFF-23** A `@unroll` loop is followed as its copies, in both modes.

## Derivatives written by hand

* **DIFF-24** `@forward(g)` on `f` names `g`, which takes each parameter of `f` and its tangent where it takes one, and gives the change of `f`'s result.
  `@backward(h)` on `f` names `h`, which takes each parameter of `f` and its adjoint as `mut` where it takes one, then the result's adjoint, and adds to each.
* **DIFF-25** Inside a function a derivative is taken of, a call of `f` runs `f` as it is and takes its derivative from `g` or `h` alone.
  So `f` may hold what DIFF-22 refuses, and `h` runs forward and is never reversed.
* **DIFF-26** The attributes name the function by its text, and one that names no function of the right shape is `unsupported-yet` at the call that needs it.

## Buffers and textures

* **DIFF-27** A read of a buffer or a texture member is constant only where the member says so (DIFF-12, DIFF-33).
  Any other read inside a function a derivative is taken of is `unsupported-yet`, and the diagnostic says to pair the member or to hold it with `no_diff(…)`.
* **DIFF-28** `@gradient(binding.member)` on a buffer or a texture names a `mut buffer[atomic[float]]` of a binding, which reverse mode adds each read's adjoint to.
  `@tangent(binding.member)` names what forward mode reads each read's change from.
  That is a `buffer[tangent[T]]` for a `buffer[T]`, and a texture of the same type for a texture.
  The entry point lists every binding a pairing names, since the derivative reads and writes what the function never named.
* **DIFF-29** A buffer element's adjoint adds one float per float of its tangent, in order: element `i`'s `k`-th at `i * n + k` of the gradient, `n` floats an element.
  A read of a buffer whose element's tangent holds a half is `unsupported-yet`.
* **DIFF-30** A `load` of a `texture_2d` at level 0 adds its adjoint to the texel it read, and a `sample` at level 0 to the texels it weighs, at their weights.
  Texel `(x, y)`'s channel `c` is element `(y * width + x) * channels + c` of the gradient.
  A linear sample's change follows its coordinate too, by the slope between those texels; a nearest sample's does not.
  A `gather` adds each component's adjoint to the one channel of its texel it read, and its coordinate moves none of them between texels.
  A comparison sample at level 0 changes with its coordinate alone under a linear filter, by the slopes of the four comparisons `gather_compare` gives.
  Its texels and its reference move it nowhere but where it jumps.
* **DIFF-31** A sample's and a gather's filter and address modes are those of a static sampler, which says them at compile time.
  A sample through a sampler bound at run time is `unsupported-yet`, and so is one at another level, with a bias, an offset or explicit gradients, or through an anisotropic sampler.
* **DIFF-32** The adds are atomic, and their order is unspecified: a sum of floats may round differently from one run to the next.
* **DIFF-33** `@no_diff` on a texture member holds its texels constant, and its sample's change still follows its coordinate.
* **DIFF-34** An entry point that declares `subgroups` shares a gradient's add through the subgroup.
  Where every invocation that reaches the add adds to one element, the subgroup sums their adjoints and one invocation adds the sum.
  The result is the same as each adding its own, up to the order of DIFF-32.
  Those subgroup operations stand wherever the add does, since each target defines them over the invocations that reach them; CHK-377 does not judge them.
  A test never shares, since it runs on no device.

## Places

* **DIFF-35** An assignment to a local, to a field or element of one, and through a swizzle carries its value's change to the place, in both modes.
  An assignment to a part of a place whose index has an effect is `unsupported-yet`.
