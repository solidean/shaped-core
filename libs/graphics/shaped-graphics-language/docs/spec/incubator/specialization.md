# Specialization

*Incubator: not normative.*

## The idea

**A function may hold several bodies, tried top to bottom, and the first whose guard holds is the one compiled.**
A guard is a constant `bool` over what a compile knows: the target's language and API, the device features the host reports, and the program's options.
A body is SGL, or the target's own text in a tagged string: inline target code, as inline assembly is to a C compiler.

One mechanism thus covers three things SGL lacks or does by hand:

* **Inline target code** — SER, NVAPI intrinsics, a Metal-only builtin, anything SGL has not named yet ([feat-sgl-inline-target-code](../../issues/feat-sgl-inline-target-code.md)).
* **Branching on a feature** — a fast path where a device has one, and a portable one where it does not.
  [feat-sgl-scoped-require-and-if-feature](../../issues/feat-sgl-scoped-require-and-if-feature.md) is that gap.
* **Per-target choices the compiler makes in C++ today** — the native float atomic add is one ([plan.cc](../../../src/shaped-graphics-language/emit/impl/plan.cc), `native_float_atomics`).

```sgl sketch
fun add(a: mut atomic[float], v: float) -> float:
    when has(float_atomic_add) and target.api == .vulkan:
        "hlsl
            $result = sgl_atomic_add_float($a, 1, 0, $v);
        "
    when has(float_atomic_add) and target.api == .metal:
        "msl
            $result = atomic_fetch_add_explicit(&$a, $v, memory_order_relaxed);
        "
    else:
        // the compare-exchange loop every target has, in SGL
```

### Guards

**A guard is an ordinary constant expression, and only the guard decides.**
A body is never skipped because it fails to check, as C++'s SFINAE skips one; a body whose guard holds and which is wrong is an error.
Every SGL body is checked on every compile, whether it is chosen or not, as both branches of an `if` on an option are today.

What a guard reads:

* `target.language` — `.hlsl`, `.wgsl` or `.msl` today; `.glsl`, `.spirv` and `.dxil` when SGL emits them.
* `target.api` — `.dx12`, `.vulkan`, `.metal` or `.webgpu`.
* `has(f)` — the device has sg feature `f`, as the host reports when it compiles.
* Any option, and any constant built from one.

**A text body's tag is its language**, and it adds `target.language == tag` to its guard.
So HLSL for vulkan and GLSL for vulkan are two bodies under one `target.api == .vulkan`, and the one the emitter can write is chosen.
A guard over the API alone and several tagged strings inside it is the common case, which is why the tag carries the language rather than the guard.

### Features

**`has(f)` makes `f` an optional feature**: used where the device has it, and never required of one.
That is new beside `require`, which refuses a device without the feature.

* A body under `has(f)` is granted `f`, as a `require` in it would grant it; that is the scoped grant the feature epic is missing.
* The features the host reports join the options in what keys a compiled text: in slib's cache, and in a frozen bundle.
* `sgl describe` reports an optional feature apart from a required one, so the host asks for it where the device has it rather than failing where it does not.

slib does exactly this for one feature already: it hands `float_atomic_add` to the compiler as an option.
That is `k_native_float_atomic_add_option` in [shader_asset.cc](../../../../shaped-shader-library/src/shaped-shader-library/shader_asset.cc).
Specialization makes that the rule for every feature, rather than one option per feature.

**A guard names sg features only**, never a target's extension string.
A vendor capability reaches a shader through sg first, which keeps sg's rule that a feature is a whole code path a device has or has not.
That is where SGL and sg meet: a text body makes the shader half of SER possible without the compiler knowing it, and enabling it on the device stays sg's.

### The signature is the contract

**Callers see one function, and what they are checked against is its declaration, never the chosen body.**
Otherwise a uniformity error, a footprint or a stage restriction would differ by target, and one check would no longer judge every target.

* Parameter types and their marks (`mut`, `out`) are the signature's, and a text body names them by placeholder.
* Effects and uniformity are declared on the function with the registry's own vocabulary: `@pure`, `@reads_only`, `@barrier`, `@derivatives`, `@subgroup`, and whether the result is uniform.
  Undeclared means impure, needing no uniform control flow, with a result that varies.
* An SGL body is checked against the declaration: a body of a `@pure` function that writes memory is an error, and so is one that takes a derivative where `@derivatives` is not declared.
* `@stages(…)` is the function's too, so the weakest body decides it: the float add's loop never leaves a helper invocation, which keeps every body of it out of the pixel stage.
* **A text body reaches GPU memory only through a resource parameter**, whose mark says read or write, as a clobber list says what inline assembly touches.
  So the footprint stays exact with no help from the text.

### Text bodies

* **They are only allowed in a specialized function**, whose signature is the contract above.
  At statement level an `if` on a constant already leaves its dead branch out of the text and the footprint (CHK-355), so `if has(subgroups):` needs nothing new.
* **They expand inline at the call**, as a builtin's custom writer does, rather than becoming a helper function in the target text.
  WGSL takes no storage buffer as a parameter, and a resource parameter is what the hazard model rests on.
* **`$name` is a parameter**, as the argument written, bound to a local first where it is no plain name, so a placeholder used twice evaluates once.
  A `mut` parameter is a place the text may assign.
  `$result` is a local of the result type the text assigns; a function without one has none.
* **Every name the text writes is reserved** from the program's names in that language (EMIT-15), read from the text rather than listed by hand.
* **A preamble per target**, deduplicated across calls, holds what the text needs ahead of the entry point.
  That is an `enable` directive, an `#include`, or a declaration such as the float add's `[[vk::ext_instruction]]`.
* `#line` reaches into the text as into everything else (EMIT-182), so DXC's error in a text body names the `.sgl` line.

### Types

A type may be specialized too, which SER needs for its hit object:

```sgl sketch
struct hit_object:
    when target.api == .dx12:
        "hlsl
            dx::HitObject
        "
    else:
        // an SGL struct holding a hit, for the bodies that fall back
```

A text type is opaque and local-only, as a ray query is: no field is reachable, no GPU memory holds it, and no stage edge carries it.
Only the bodies of specialized functions take it apart.

### Coverage

* A compile in which no body's guard holds is an error at the entry point that reaches the call, as `target-lacks-feature` is today.
* A specialized function whose last body is guarded has no fallback, which a lint reports.
* The interpreter is a target whose language is SGL itself, so it runs the first SGL body whose guard holds, and a test reaching only text bodies is an error.
  A test may set the features `has` sees, so every SGL body runs on the CPU, the fallback above all.

### Text bodies are a lane, not a home

What proves itself in a text body moves into the language: a builtin, a type, a feature.
SER is the expected path: a hit object in a text body first, then `hit_object` in the vocabulary of [raytracing-futures.md](raytracing-futures.md).
Some of the prelude's own builtins may go the other way, from a C++ custom writer into a specialized prelude function, which is where this starts.

## Walkthroughs

**The float atomic add, first.**
The C++ special case becomes a specialized prelude function, as in the first sketch above.
It proves guards on API and feature, text bodies with a preamble, an optional feature, and the SGL fallback, against GPU tests that exist already, with no sg work.
The fallback has to reach the `uint` bits of an `atomic[float]`, which no SGL spelling does yet, so an `@internal` builtin for that view comes with it.

**SER, second.**
A specialized `hit_object` type and a specialized `reorder` function, with a `"hlsl` body under `.dx12` and an empty SGL fallback, since reordering is a hint.
Under `.vulkan` the `"hlsl` body needs SPIR-V instructions of its own, since DXC lowers no SER to SPIR-V.
sg needs the feature, the device extension and an SM 6.9 profile from slib's DXC edge first.

**Opacity micromaps need no text body.**
sg builds them on vulkan already, and a trace honors one with no change to the shader.
What SGL lacks is the 2-state ray flag and `opacity_micromap` among the features a `require` names, which are an ordinary builtin and an ordinary feature.

## What it touches

* The parser and the AST: a function or a struct whose body is guarded bodies, and the tagged string as a body.
* The check pass: guards as constants, `target` and `has` as names, the declaration as the contract, SGL bodies checked against it, and coverage.
* Flatten: the chosen body inlines like any other, and a text body is a leaf whose effects are declared.
* The emitters: placeholders, `$result`, the preamble, reserved names read from the text, and `#line` inside it.
* The interpreter: SGL bodies by the same guards, with the features a test sets.
* `sgl describe` and slib: optional features, and the reported features in the cache key and in frozen bundles.
* sg: each feature a guard names, beginning with SER's.

## Already fixed by the syntax

* A tagged opener, a quote followed by one name on a line with children, tokenizes today and draws `reserved-string-opener` (STR-31, STR-45).
  [string-family.md](string-family.md) reserves it for language-tagged strings, which a text body is.
* `$name` and `$(expr)` interpolation are tokenized in every double-quoted string (STR-37), so placeholders need no new token.
* `"""` and `"""lang` are reserved for raw strings, a text body with no placeholders at all.
* `when` is no keyword yet.

## Open

* The syntax: `when` arms inside one declaration, as sketched, or one `@when(…)` declaration per body, top to bottom in one file.
* Whether a text statement may stand in an ordinary body, reaching any local in scope, as GCC's `asm` does.
* `$result`, `$return` or a last line that is the value.
* What `target.api` is in the interpreter, and whether a test names features by `has` or by a set it states.
* Whether the declared effects may be inferred from the SGL bodies where every body is SGL, so only a function with a text body declares them.
* Whether an optional feature's reported value comes from the device, or from what the host chooses to enable, which may be less.
* Whether two bodies may differ in what they need of a pipeline layout, such as a binding the fallback uses and the native body does not.
