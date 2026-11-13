# Float Guarantees as a Ladder

*Incubator: not normative.*

**Specified since 2026-11-05**: the attribute and an entry point's level (CHK-435), the lint (CHK-436), what each target keeps (EMIT-185) and the licenses (EVAL-6).
What stays here is metal's denormals, levels finer than the entry point, and the open questions below.

## The idea

**A float means exactly what the interpreter computes, and each level of a ladder licenses a target to depart from that in named ways.**
EVAL-6 calls a `float` a 32-bit IEEE number, and no target compiles shaders that way by default: DXC marks every operation `fast`, Metal compiles fast math, and WGSL may assume no NaN or infinity.
Asking for strictness everywhere costs every shader for what few need, and it has a sharper cost too.
On an Intel Arc B580, DXC's `-Gis` left shaped-viewer's pipelines stuck in the driver's compiler for minutes.
[question-sgl-float-semantics-on-every-target](../../issues/.closed/2026/11/question-sgl-float-semantics-on-every-target.md) has the measurements.
So strictness is asked for where it is needed, and paid for only there.

The ladder, from loosest to strictest; each level licenses what is listed and every level below it is stricter:

| level | the target may, beyond the exact meaning |
|---|---|
| `fast` | assume no NaN or infinity arises, ignore the sign of zero, reassociate, contract into a fused multiply-add, divide through a reciprocal, flush denormals |
| `specials` | reassociate, contract, divide through a reciprocal, flush denormals |
| `precise` | flush denormals |
| `ieee` | nothing, but the builtins' own stated precision |

**`fast` is the default for every stage**, the precision any HLSL a shader is ported from already had.

**A function states the least level it needs, and an entry point takes the most any function it reaches states.**
`@float(.precise)` on a robust orientation predicate or a compensated sum is written once, beside the code that relies on it, and every caller inherits it.
An entry point may state one itself, which raises its own and is the only place a level can be asked for while the compile is per entry point.
`describe` reports an entry point's level and the function that raised it, so a slower pipeline or a stalled driver is traced to the line that asked.

**A level a target cannot keep is refused, as a missing feature is refused** (CHK-322).
Here is how each target would keep each level:

| level | dx12 | vulkan | metal | webgpu |
|---|---|---|---|---|
| `fast` | DXC's default | DXC's default | fast math, which the text states (EMIT-262) | the default |
| `specials` | `-Gis` | `SignedZeroInfNanPreserve`, with `shaderSignedZeroInfNanPreserveFloat32` | safe math, which the text states | refused |
| `precise` | `-Gis` | the same, and `NoContraction` on each operation | safe math, which the text states | refused |
| `ieee` | `-Gis -denorm preserve`, Shader Model 6.2 on | the same, and `DenormPreserve` with `shaderDenormPreserveFloat32` | refused: safe math flushes denormals | refused |

**WebGPU refuses every level above `fast` by default, since nothing a shader says reaches Tint's or Naga's float handling.**
A context or a pipeline may opt in to weaker semantics there, and its entry points then compile at `fast` whatever they state, with a warning that names what was dropped.
The opt-in is the host's, never the shader's: the shader states what it needs, and only the code that picked WebGPU knows whether running without it is acceptable.

**Finer than the entry point comes later.**
`precise` in HLSL and `NoContraction` in SPIR-V are per operation, so a level could hold for one function's code alone, once the emitters write it per operation.
dx12's `-Gis` is per compile and Metal's math mode is per text, which is why the entry point is the unit until then.

## What it touches

* The checker: `@float(...)` on a function and an entry point, the maximum over what an entry point reaches, and the refusal where a target cannot keep it.
* The spec: EVAL-6 becomes the exact meaning, and each level a license against it, stated as an as-if rule.
* slib's SGL edge: the flags each target's compile is handed, `-Gis` and `-denorm` for DXC.
* The MSL emitter: the math mode, which the text states as a pragma the Metal compiler takes over its own options (EMIT-262).
* The emitters: the SPIR-V execution modes through `[[vk::ext_execution_mode]]`, and `NoContraction` or `precise` per operation for a scope later.
* sg: the vulkan device features each level needs, reported as features are, and the WebGPU opt-in on a context or a pipeline.
* `describe`: an entry point's level and what raised it.
* The linter: a NaN or infinity test, `isnan`, `isinf` or `x != x`, in code at `fast` is one the compiler may delete, and is a warning that names the level it needs.

## Already fixed by the syntax

* An attribute takes an enum case as its argument, as `@unroll(false)` and `@padding(.none)` already do, so `@float(.specials)` needs nothing new.
* Features already propagate from a function to the entry points that reach it, which is the same walk a level needs.

## Open

* **What `fast` means for a NaN that does arise.** Under LLVM's fast-math flags a NaN where none was assumed is poison, not merely an imprecise value, so a branch on it may go either way.
  How that meets the robustness profiles of LEGAL-65, and whether `report` should raise the level of what it guards, is not decided.
* **The builtins' precision** is a separate axis: `sin`, `exp` and `sqrt` keep what each target's rules allow at every level.
  It wants a table per target rather than a rung of the ladder.
* **Tests run at `ieee`**, since the interpreter is exact, while the GPU runs what the entry point states.
  Whether a test that asserts on a NaN or an infinity in a function below `specials` should warn is open.
* **A driver that miscompiles the strict text** is outside the promise, which is what the toolchain is asked for.
  Intel's dx12 driver returned 0 for a strict `0 * inf` under `-Gis`, and such a finding belongs in docs/bugs-external once reproduced standalone.
