# Used Access

## The idea

**SGL reports the access each pipeline actually performs on a binding, beside the access the binding declares.**
A `mut image2d` that one entry point only loads from is a read to that pipeline, whatever the group says.
A group is often declared once and bound to several pipelines, so a declared `mut` is the union of what they do, and each pipeline alone does less.

sg infers a dispatch's hazards from the bound view's class today, so an image counts as written even when the shader only reads it.
With the used access in a pipeline's reflection, sg's barrier inference reads the narrower one instead: two dispatches that only read the same image need no barrier between them.
The sg side is tracked in [shaped-graphics' TODO](../../../../shaped-graphics/docs/TODO.md).

## What it touches

* Checking: the used access of each binding member, per entry point, after inlining, since every function inlines.
* `sgl describe` and the compiled shader's reflection: a used access per binding, per entry point.
* sg: `shader_access_of` reads the pipeline's used access rather than the view class.

## Already fixed by the syntax

* The declared access is the member's access word — unmarked, `mut` or `out` — and it stays what the group layout carries.
  WebGPU validates a bind group against its layout, so the layout keeps the declared access, and only the hazards narrow.

## Open

* Whether a buffer narrows the same way, since a buffer's declared access also picks SRV or UAV on dx12 and cannot narrow there.
* Whether an `out` image that a pipeline never touches is reported as unused, so its binding costs no barrier at all.
