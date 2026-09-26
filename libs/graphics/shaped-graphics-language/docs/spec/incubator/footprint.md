# Footprint

## The idea

**A pipeline carries its footprint: what its code actually does to each binding, per stage.**
A binding's `access` is what the group layout permits, and a group is often declared once and bound to many pipelines, so it is the union of what they all do.
One pipeline alone usually does less: a `mut buffer` one entry point only loads from is a read to that pipeline, and a member it never names is untouched.
SGL knows this precisely, because every function inlines and the analysis runs on the code each entry point really executes.

sg infers a dispatch's hazards from each bound view's `bound_as` today, so an image counts as written even where the shader only reads it.
With a footprint, sg joins the group's bound resources with the pipeline's footprint at dispatch, and the hazards are exactly the ones the code causes.
The sg side is tracked in [shaped-graphics' TODO](../../../../shaped-graphics/docs/TODO.md).

**The footprint is spelled in the barrier tracker's own vocabulary**, not in `access_mode`.
One entry is a binding, the stages it is touched in, and the `sg::access_flags` it is touched with in each of them.
It is per stage because some APIs want barriers and residency declared per stage.
An entry that is empty means untouched, and costs no barrier at all.
The name is chosen to stay clear of `access`, which is a binding's permission, and of `usage`, which is a resource's creation flags.

### Arrays and bindless

**What an array's elements are used for cannot be analysed, so there the footprint needs the caller's cooperation.**
A dynamic index into a binding array reaches elements chosen at runtime.
The conservative answer — every element, written — degrades a bindless dispatch to barriers so broad the feature stops paying for itself.
Sometimes a global barrier is acceptable, and often it is not.

So for an array, the footprint reports *whether and how* the code indexes it: the stages, the access flags, and whether the index is dynamic.
Which elements that reaches stays the caller's to declare per dispatch, as sg's `declare_array_buffer_access` / `declare_array_texture_access` already require.
The footprint then bounds the declaration: declaring a write on an array the code only reads is a bug sg can catch.

## What it touches

* Checking: the footprint of each binding member, per entry point and per stage, after inlining.
* `sgl describe` and the compiled shader's reflection: a footprint beside the bindings.
* sg: a pipeline carries its footprint, and barrier inference joins it with the bound resources at dispatch.
  The groups' hazard records then carry no access at all, only the resource, its range and `bound_as`.
* HLSL and WGSL: reflection cannot see the code's behaviour, so their footprint is the conservative one, derived from `binding::access` and the stage visibility.
  That is today's behaviour, spelled explicitly.

## Already fixed by the syntax

* The declared access is the member's access word — unmarked, `mut` or `out` — and it stays what the group layout carries as `binding::access`.
  WebGPU validates a bind group against its layout, so the layout keeps the declared access, and only the hazards narrow.

## Open

* SGL may come to name the per-dispatch array declaration itself — tentatively `access` — rather than leaving it to the host alone.
* Whether a footprint entry distinguishes a uniform dynamic index from a non-uniform one, which some backends pay for differently.
