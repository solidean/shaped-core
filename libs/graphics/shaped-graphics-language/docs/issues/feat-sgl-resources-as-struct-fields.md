---
created: 2026-11-22
tags: [porting-feedback]
refs:
  - issue: feat-sgl-resources-as-let-values
---

# A struct field holds a texture, a buffer or a binding array's element

A port of a bindless renderer wanted to group the resources one material or one pass reads into a struct and hand it around.
A struct field of a resource type is refused today.

Wanted, under the rule CHK-536 set for a `let` of a resource:

- **Inlining and scalarizing undo it completely**: every read of the field resolves at compile time to one binding member, or to one element of a binding array whose index is evaluated once.
- A struct holding a resource is a value of the function bodies alone: no buffer, constant block, stage edge or workgroup memory holds one.
- Where the field cannot be resolved, such as a struct chosen at run time or a field assigned another resource, it is an error naming why, never a target's own resource variable.
- The flattener's `resource_stand_in`, which the `let` and the parameter share, is the substitution a field would reuse.
