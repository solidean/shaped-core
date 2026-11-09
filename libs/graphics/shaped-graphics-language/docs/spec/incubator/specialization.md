# Specialization

*Incubator: not normative.*

## The idea

**`const if` is an `if` whose conditions are constants, so only the branch taken is compiled.**
A condition reads what a compile knows: the target's language and API, the device features the host reports, the program's options, and a generic's parameters.
A function whose whole body is such a chain may hold the target's own text in a branch: inline target code, as inline assembly is to a C compiler.

One mechanism thus covers four things SGL lacks or does by hand:

* **Inline target code** — SER, NVAPI intrinsics, a Metal-only builtin, anything SGL has not named yet ([feat-sgl-inline-target-code](../../issues/feat-sgl-inline-target-code.md)).
* **Branching on a feature** — a fast path where a device has one, and a portable one where it does not.
  [feat-sgl-scoped-require-and-if-feature](../../issues/feat-sgl-scoped-require-and-if-feature.md) is that gap.
* **Branching on a generic's parameter** — `const if T.is_float:` in a template, as C++'s `if constexpr`.
* **Per-target choices the compiler makes in C++ today** — the native float atomic add is one ([plan.cc](../../../src/shaped-graphics-language/emit/impl/plan.cc), `native_float_atomics`).
  It reads the compile's device features already, as `has` does.

```sgl sketch
@stages(.compute, .raygen, .miss, .closest_hit, .callable)
fun add(a: mut atomic[float], v: float) -> float:
    const if has(.float_atomic_add) and target_api == .vulkan:
        "hlsl
            $result = sgl_atomic_add_float($a, 1, 0, $v);
        "
    else if has(.float_atomic_add) and target_api == .metal:
        "msl
            $result = atomic_fetch_add_explicit(&$a, $v, memory_order_relaxed);
        "
    else:
        // the compare-exchange loop every target has, in SGL
```

### `const if`

Specified so far: `const if` itself (AST-157, CHK-425), the target and device it reads (CHK-426), and text bodies in their first form (STR-46, AST-158, CHK-427).
So is discarding what an instance rules out (CHK-429, CHK-430): `const if T == float:` checks one branch per instance.
What stays here is what it grows into: asking more of a type than which it is, and the rest of text bodies below.

**`const if` heads an ordinary `if` chain, and every `else if` of the chain is constant too.**
The chain's siblings pair as an `if`'s do (AST-50, AST-51), and a condition that is not constant is an error.
It may stand wherever an `if` may, a function's whole body or one statement among others.

What a condition reads:

* `target_language` — `.hlsl`, `.wgsl` or `.msl` today, and `.sgl` in a test; `.glsl`, `.spirv` and `.dxil` when SGL emits them.
* `target_api` — `.dx12`, `.vulkan`, `.metal` or `.webgpu`, and `.none` in a test.
* `has(.f)` — the device has sg feature `f`, as the host reports when it compiles.
* Any option, and any constant built from one.
* A generic's parameters, and whether two types are one, `T == float` (CHK-429).
  More than that, whether `T` is a vector or of which scalar, is what [types-as-values.md](types-as-values.md) would let a condition ask.

They are `target_api` rather than `target.api` because the prelude's names are shadowable, and a program's own `target` struct is common.

### What is checked

**A branch is checked unless its condition is false whatever the target, the features and the options are.**
The condition is evaluated with `target`, `has(…)` and every option unknown, in three-valued logic:

* A condition over a generic's parameters alone is known per instance, so the branch it rules out is never checked for that instance, as C++ discards it.
* A condition that reads `target`, `has` or an option is unknown, so every branch it guards is checked on every compile, the one not taken included.
  So a fallback a dx12 author never runs still fails on their machine, and one check keeps judging every target.
* `else` after a branch whose condition is known true is ruled out, and so is everything after it.

A branch is never skipped because it fails to check, as C++'s SFINAE skips an overload.

**The target and the device's features are inputs of a compile, as options are** (CHK-426).
So a source is checked once per target, and `const if target_api == .vulkan` folds exactly as an `if` on an option does (CHK-356).
They are substituted where an inlined body reads them, rather than where a const compiles, and a const that reads them is refused.
That is what lets a checked layer of modules serve every target.
"Checked" above is the check pass: names, types and the contract, for every branch the rule keeps.
What is judged of the inlined tree, uniformity, the footprint and the features an entry point needs, is judged of the branch taken alone.
So a compile for one target judges its own branch, and a compile of every target, as the corpus runs, judges them all.

### Features

**`has(.f)` makes `f` an optional feature**: used where the device has it, and never required of one.
That is new beside `require`, which refuses a device without the feature.

* A branch under `has(.f)` is granted `f`, as a `require` in it would grant it; that is the scoped grant the feature epic is missing.
* slib hands every feature a context has to an SGL compile as one option, `k_device_features_option` in [shader_asset.cc](../../../../shaped-shader-library/src/shaped-shader-library/shader_asset.cc).
  So the features key a compiled text in slib's cache, and in a frozen bundle, as the options do.
* `sgl describe` checks for no device, so it reports what the fallback needs; reporting an optional feature apart from a required one is open below.

**A condition names sg features only**, never a target's extension string.
A vendor capability reaches a shader through sg first, which keeps sg's rule that a feature is a whole code path a device has or has not.
That is where SGL and sg meet: a text body makes the shader half of SER possible without the compiler knowing it, and enabling it on the device stays sg's.

### Text bodies

**A tagged string in the target's language is a text body**, and it stands only as a whole body:

* the whole body of a function;
* or the whole branch of a `const if` that is itself the function's whole body, at any depth of such chains.

A run of tagged strings, one per language, counts as one text body, of which the emitter writes the one in its language.
A branch taken with no tagged string in the language being written is an error, never a fall-through to the next branch.
So a condition states the APIs its text serves, and a new target that meets a condition meets an error rather than a slower path chosen in silence.

**Every body of the function honors one contract, its declaration**, and callers are checked against that, never against the branch taken.
Otherwise a uniformity error, a footprint or a stage restriction would differ by target.

* Parameter types and their marks (`mut`, `out`) are the signature's.
* Effects and uniformity are attributes of the `fun`, in the registry's own vocabulary: `@pure`, `@barrier`, `@subgroup` and `@derivatives` (CHK-431).
  `@pure` also says the result is as uniform as the arguments.
  Undeclared means impure, needing no uniform control flow, with a result that may differ between invocations.
* An SGL branch is checked against the declaration: one of a `@pure` function that writes memory is an error, and so is one that takes a derivative where `@derivatives` is not declared.
* `@stages(…)` is the function's too, so the weakest branch decides it: the float add's loop never leaves a helper invocation, which keeps every branch of it out of the pixel stage.
* **A text body reaches GPU memory only through a resource parameter**, whose mark says read or write, as a clobber list says what inline assembly touches.
  So the footprint stays exact with no help from the text.

How the text is written:

* **It expands inline at the call**, as a builtin's custom writer does, rather than becoming a helper function in the target text.
  WGSL takes no storage buffer as a parameter, and a resource parameter is what the hazard model rests on.
* **`$name` is a parameter**, as the argument written, bound to a local first where it is no plain name, so a placeholder used twice evaluates once.
  A `mut` parameter is a place the text may assign.
* **`$result` is a local of the result type, which the text assigns**, so one rule serves every language and a body of several statements.
* **The placeholders are checked, although the text is not**: a `$name` naming no parameter is an error.
  `$result` stands in a text body exactly when the function has a result, and no parameter is named `result`.
* **Every name the text writes is reserved** from the program's names in that language (EMIT-15), read from the text rather than listed by hand.
* **A preamble per target**, deduplicated across calls, holds what the text needs ahead of the entry point.
  That is an `enable` directive, an `#include`, or a declaration such as the float add's `[[vk::ext_instruction]]`.
* `#line` reaches into the text as into everything else (EMIT-182), so DXC's error in a text body names the `.sgl` line.

### Types

A struct whose only top-level form is a `const if` chain is specialized the same way, which SER needs for its hit object.
Each branch holds fields, or a text body that spells the type:

```sgl sketch
struct hit_object:
    const if target_api == .dx12:
        "hlsl
            dx::HitObject
        "
    else:
        // fields of an SGL struct holding a hit, for the branches that fall back
```

Specified as CHK-432: the check judges the type by its SGL branch, and a compile that spells it as text refuses a field read or a construction that reaches it.
It is local-only on every target, as a ray query is: no GPU memory holds it, and no stage edge carries it.
So the SGL branches of specialized functions read its fields, and their text bodies take it apart where it is the target's own.

### Coverage

* A compile that takes a branch with no body for its language is an error at the entry point that reaches the call, as `target-lacks-feature` is today.
* A function whose whole body is a `const if` chain without an `else` has no fallback, which a lint reports.
* The interpreter is a target whose language is SGL itself, so a branch it takes must be SGL, and a test reaching only text is an error.
  A test sees `.none`, `.sgl` and no feature today (CHK-426), so it runs the fallback; letting it set the features `has` sees is open below.

### Text bodies are a lane, not a home

What proves itself in a text body moves into the language: a builtin, a type, a feature.
SER is the expected path: a hit object in a text body first, then `hit_object` in the vocabulary of [raytracing-futures.md](raytracing-futures.md).
Some of the prelude's own builtins may go the other way, from a C++ custom writer into a specialized prelude function, which is where this starts.

## Walkthroughs

**The float atomic add, first.**
The C++ special case becomes a specialized prelude function, as in the first sketch above.
It proves conditions on API and feature, text bodies with a preamble, an optional feature, and the SGL fallback, against GPU tests that exist already, with no sg work.
The fallback has to reach the `uint` bits of an `atomic[float]`, which no SGL spelling does yet, so an `@internal` builtin for that view comes with it.

**SER, second.**
The first half is done: prelude `maybe_reorder(hint, bits)`, a text body under `has(.shader_execution_reordering)` with an empty fallback.
sg reports the feature on dx12 where the Agility SDK's runtime gives Shader Model 6.9, and slib compiles an entry point that uses it at 6.9.
It reports it on vulkan where `VK_EXT_ray_tracing_invocation_reorder` reorders, and the vulkan branch is a builtin, since its instruction must be declared outside the body.
GPU tests trace through a reordering raygen on dx12 and on vulkan, each really reordering, and through the fallback on a device without the feature.
The second half is a compiler construct rather than a text type, since only the compiler knows a ray type's table indices.
`trace(…, reorder = true)`, or `reorder = hint, reorder_bits = n`, traces into a hit object, regroups by it and invokes it on dx12 and vulkan (CHK-433), and is the plain trace elsewhere.
A first-class `hit_object` lets a raygen read a hit before it runs its shaders (CHK-434).
It is gated on the feature rather than specialized: no other target can trace without running them, so the program writes the plain `trace` beside it.
Under `.vulkan` it is SPIR-V instructions of its own, since no DXC lowers SER to SPIR-V; neither the pinned 1.9.2602 nor the Vulkan SDK's does (EMIT-225).

**Opacity micromaps need no text body.**
sg builds them on vulkan already, and a trace honors one with no change to the shader.
What SGL lacks is the 2-state ray flag and `opacity_micromap` among the features a `require` names, which are an ordinary builtin and an ordinary feature.

## What it touches

Done: `const if`, constant conditions, `target_api`, `target_language` and `has`, the grant, slib's device features.
Discarding is done too: a type comparison, and the three-valued decision that leaves an instance's ruled-out branches unchecked (CHK-430).
Text bodies in their first form are done too: the tagged string as a body, its placement, placeholders and `$result`, a `flat_call` with `text`, and the emitters writing it inline.
So is their declared contract, `@pure`, `@barrier`, `@subgroup` and `@derivatives` on the `fun`, which the uniformity pass reads in place of the text (CHK-431).
So are text types, a struct spelled per compile in the target's text or as its SGL fields (AST-160, CHK-432).
So are a buffer as a parameter, which a text body names as it names a texture (CHK-366), and `#line` inside the text (EMIT-182).
Left:

* The check pass: SGL branches checked against the declared contract, so a fallback of a `@pure` function that writes memory is an error, and coverage.
* The attributes' stages: `@derivatives` keeps a call to the pixel stage, and `@subgroup` to a device with subgroups, which `@stages` and `has` say by hand today.
* Generics: questions of a type beyond its identity, such as whether it is a vector, and its scalar where it is a scalar itself.
* The emitters: reserved names read from the text.
  The preamble is done (CHK-428), a file's per language; one per API, such as vulkan's alone among the HLSL targets, is open.
* Text types beyond their first form (CHK-432): a condition on an option.
* The interpreter: the features a test sets.
* `sgl describe`: optional features.
* sg: each feature a condition names, beginning with SER's.
* raytracing.sgl's `by_target`, the flat tree's own select between the native and the emulated trace, which a `const if` on `target_api` could replace once `describe` reports per target.

## Already fixed by the syntax

* A tagged opener, a quote followed by one name on a line with children, tokenizes today and draws `reserved-string-opener` (STR-31, STR-45).
  [string-family.md](string-family.md) reserves it for language-tagged strings, which a text body is.
* `$name` and `$(expr)` interpolation are tokenized in every double-quoted string (STR-37), so placeholders need no new token.
* `"""` and `"""lang` are reserved for raw strings, a text body with no placeholders at all.
* `const` and `if` are keywords already, and `else if` shows the shape of a two-word keyword form.

## Open

* How a test states the features `has` sees, so the native branch runs on the CPU too.
* Whether `sgl describe` describes per target, which a `const if` on `target_api` would need for the footprint it reports.
* Whether the declared effects may be inferred where every branch is SGL, so only a function with a text body declares them.
* Whether an optional feature's reported value comes from the device, or from what the host chooses to enable, which may be less.
* Whether two branches may differ in what they need of a pipeline layout, such as a binding the fallback uses and the native body does not.
* Whether a `const if` among other statements, rather than as a whole body, discards its ruled-out branch from the footprint as an `if` on an option does (CHK-355).
