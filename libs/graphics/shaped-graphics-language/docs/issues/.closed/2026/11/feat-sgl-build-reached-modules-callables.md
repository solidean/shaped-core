---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - issue: chore-sgl-review-callable-section
---

# A program builds the callables of the modules it reaches

Since CHK-562 a program's callable section lists the callables tables of every module it reaches.
But CHK-350 keeps a used module's entry points out of the program's compile, so a raygen that reaches a module's `@callable` cannot have that callable built with it.
A shader package therefore cannot yet build a pipeline whose section holds a module's table.

Quest3D works around it by building each such callable from its module's own file under the raygen file's handle:
`hybrid_trace_light.sgl:rcall:hybridPickLocalLightCall:@hybrid_trace.hybridPickLocalLightCall`.
That holds for a callable that calls no table.
A callable built that way that calls another table takes the module's own callable section, not the raygen's, so its indices can differ from the pipeline's.

Either build the reached modules' callables as entry points of the program that reaches them, so each sees the program's section.
Or refuse a module's callable that calls a table when it is built from its own file (`unsupported-yet`) until then.
