---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/float-guarantees.md
  - issue: question-sgl-float-semantics-on-every-target
---

# Float guarantees as a ladder: fast, specials, precise, ieee

The design is [incubator/float-guarantees.md](../spec/incubator/float-guarantees.md), agreed on 2026-11-05.
A function states the least level it needs with `@float(...)`, an entry point takes the most it reaches, and `fast` is the default for every stage.
A level a target cannot keep is refused; WebGPU refuses every level above `fast` unless the host opts in on a context or a pipeline.
It answers [question-sgl-float-semantics-on-every-target](.closed/2026/11/question-sgl-float-semantics-on-every-target.md), which holds what was measured on dx12.

The children are the pieces, in the order they are worth doing:
the attribute and its propagation, each target's compile flags, the vulkan execution modes and device features, the WebGPU opt-in, and the NaN-test lint.
Per-scope levels wait for the emitters to write `precise` and `NoContraction` per operation.
